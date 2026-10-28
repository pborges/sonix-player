#define _GNU_SOURCE 1

#include "absdl.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "src/system/abs/abs.h"
#include "src/system/core/json.h"
#include "src/system/core/utils.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastdl.h"

#define SIDECAR ".abs.json"
#define COVER_NAME "cover.jpg"
#define COVER_SIZE 600
#define TIMEOUT_SECS 20
#define DETAIL_LIMIT (2 * 1024 * 1024)
#define CHUNK (64 * 1024)
// Left free on the card after a book, for everything else that writes there.
#define SPACE_MARGIN (64LL * 1024 * 1024)
#define WALK_DEPTH 6

typedef struct {
	char name[256]; // on the card, inside the book's folder
	char ino[32];	// on the server
	long long size;
	double start; // seconds into the book where this file begins
	double duration;
} dlfile_t;

typedef struct {
	absdl_entry_t e;
	dlfile_t *files;
	int file_count;
} book_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

static book_t *books;
static int book_count;
static int book_capacity;

static char queue[ABSDL_QUEUE_MAX][40];
static int queue_len;
static char deletions[ABSDL_QUEUE_MAX][40];
static int deletion_count;

static char running[40];
static volatile bool cancel_running;
static bool walk_needed;
static unsigned serial;

static char card_root[512];

// ---------------------------------------------------------------------------
// the list of books, under `lock`
// ---------------------------------------------------------------------------

static book_t *find_locked(const char *id) {
	for (int i = 0; i < book_count; i++) {
		if (strcmp(books[i].e.id, id) == 0) {
			return &books[i];
		}
	}
	return NULL;
}

static book_t *add_locked(const char *id) {
	book_t *b = find_locked(id);
	if (b) {
		return b;
	}
	if (book_count == book_capacity) {
		int grown = book_capacity ? book_capacity * 2 : 16;
		book_t *more = realloc(books, (size_t)grown * sizeof(*more));
		if (!more) {
			return NULL;
		}
		books = more;
		book_capacity = grown;
	}
	b = &books[book_count++];
	memset(b, 0, sizeof(*b));
	snprintf(b->e.id, sizeof(b->e.id), "%s", id);
	return b;
}

static void remove_locked(const char *id) {
	for (int i = 0; i < book_count; i++) {
		if (strcmp(books[i].e.id, id) == 0) {
			free(books[i].files);
			books[i] = books[--book_count];
			return;
		}
	}
}

static void changed_locked(void) { serial++; }

// ---------------------------------------------------------------------------
// .abs.json
// ---------------------------------------------------------------------------

static void json_write_string(FILE *f, const char *s) {
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			fputc('\\', f);
			fputc(*p, f);
		} else if (*p < 0x20) {
			fprintf(f, "\\u%04x", *p);
		} else {
			fputc(*p, f);
		}
	}
	fputc('"', f);
}

static bool sidecar_write(const book_t *b, const char *server, bool complete) {
	char path[700], tmp[710];
	snprintf(path, sizeof(path), "%s/%s", b->e.folder, SIDECAR);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	fputs("{\n  \"version\": 1,\n  \"item_id\": ", f);
	json_write_string(f, b->e.id);
	fputs(",\n  \"server\": ", f);
	json_write_string(f, server);
	fputs(",\n  \"title\": ", f);
	json_write_string(f, b->e.title);
	fputs(",\n  \"author\": ", f);
	json_write_string(f, b->e.author);
	fprintf(f, ",\n  \"duration\": %.3f,\n  \"complete\": %s,\n  \"files\": [", b->e.duration, complete ? "true" : "false");
	for (int i = 0; i < b->file_count; i++) {
		const dlfile_t *d = &b->files[i];
		fputs(i ? ",\n    {\"name\": " : "\n    {\"name\": ", f);
		json_write_string(f, d->name);
		fputs(", \"ino\": ", f);
		json_write_string(f, d->ino);
		fprintf(f, ", \"size\": %lld, \"start\": %.3f, \"duration\": %.3f}", d->size, d->start, d->duration);
	}
	fputs("\n  ]\n}\n", f);
	bool ok = fflush(f) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

// Reads the .abs.json in `folder` into a book. False when there is none, or it
// is not one of ours. `folder` must not point into `out`, which is cleared.
static bool sidecar_read(const char *folder, book_t *out, bool *complete) {
	char path[700];
	snprintf(path, sizeof(path), "%s/%s", folder, SIDECAR);
	char *text = read_file_content(path);
	if (!text) {
		return false;
	}
	json_doc_t doc;
	bool ok = json_parse(text, &doc);
	if (ok) {
		int root = json_root(&doc);
		memset(out, 0, sizeof(*out));
		json_obj_str(&doc, root, "item_id", out->e.id, sizeof(out->e.id));
		json_obj_str(&doc, root, "title", out->e.title, sizeof(out->e.title));
		json_obj_str(&doc, root, "author", out->e.author, sizeof(out->e.author));
		out->e.duration = json_double(&doc, json_get(&doc, root, "duration"), 0);
		*complete = json_obj_bool(&doc, root, "complete", false);
		snprintf(out->e.folder, sizeof(out->e.folder), "%s", folder);
		int files = json_get(&doc, root, "files");
		int n = json_len(&doc, files);
		out->files = n > 0 ? calloc((size_t)n, sizeof(dlfile_t)) : NULL;
		for (int i = 0; out->files && i < n; i++) {
			int f = json_at(&doc, files, i);
			dlfile_t *d = &out->files[out->file_count++];
			json_obj_str(&doc, f, "name", d->name, sizeof(d->name));
			json_obj_str(&doc, f, "ino", d->ino, sizeof(d->ino));
			d->size = json_obj_llong(&doc, f, "size", 0);
			d->start = json_double(&doc, json_get(&doc, f, "start"), 0);
			d->duration = json_double(&doc, json_get(&doc, f, "duration"), 0);
			out->e.total += d->size;
		}
		ok = out->e.id[0] != '\0';
		if (!ok) {
			free(out->files);
			out->files = NULL;
			out->file_count = 0;
		}
		json_free(&doc);
	}
	free(text);
	return ok;
}

static long long file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

// How much of a book is on the card: whole files, and what the .part of the
// next one holds.
static long long bytes_present(const book_t *b) {
	long long done = 0;
	for (int i = 0; i < b->file_count; i++) {
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", b->e.folder, b->files[i].name);
		if (file_size(path) == b->files[i].size) {
			done += b->files[i].size;
			continue;
		}
		snprintf(path, sizeof(path), "%s/.%s.part", b->e.folder, b->files[i].name);
		long long part = file_size(path);
		if (part > 0) {
			done += part;
		}
	}
	return done;
}

// Every .abs.json under the Audiobooks folder: the books this module put there,
// whole or not, and nobody else's.
static void walk(const char *dir, int depth, book_t **found, int *count, int *capacity) {
	if (depth > WALK_DEPTH) {
		return;
	}
	char sidecar[700];
	snprintf(sidecar, sizeof(sidecar), "%s/%s", dir, SIDECAR);
	if (access(sidecar, F_OK) == 0) {
		book_t b;
		bool complete = false;
		if (sidecar_read(dir, &b, &complete)) {
			if (*count == *capacity) {
				int grown = *capacity ? *capacity * 2 : 16;
				book_t *more = realloc(*found, (size_t)grown * sizeof(*more));
				if (!more) {
					free(b.files);
					return;
				}
				*found = more;
				*capacity = grown;
			}
			b.e.done = bytes_present(&b);
			// Complete in the file and still whole on the card; a book whose
			// files were deleted by hand is a partial one.
			b.e.state = complete && b.e.done == b.e.total ? ABSDL_DONE : ABSDL_PARTIAL;
			(*found)[(*count)++] = b;
		}
		return; // a book's folder holds no other book of ours
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char child[700];
		if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
			walk(child, depth + 1, found, count, capacity);
		}
	}
	closedir(d);
}

static void audiobooks_root(const char *root, char *out, size_t size) {
	snprintf(out, size, "%s/%s", root, AUDIOBOOKDB_FOLDER);
}

// Reads what is on the card again, keeping what is only in memory: the queued
// books, and the one downloading.
static void refresh_from_card(void) {
	char root[512], dir[600];
	pthread_mutex_lock(&lock);
	snprintf(root, sizeof(root), "%s", card_root);
	pthread_mutex_unlock(&lock);
	if (!root[0]) {
		return;
	}
	audiobooks_root(root, dir, sizeof(dir));
	book_t *found = NULL;
	int count = 0, capacity = 0;
	walk(dir, 0, &found, &count, &capacity);

	pthread_mutex_lock(&lock);
	for (int i = book_count - 1; i >= 0; i--) {
		absdl_state_t s = books[i].e.state;
		if (s != ABSDL_QUEUED && s != ABSDL_RUNNING && s != ABSDL_DELETING) {
			free(books[i].files);
			books[i] = books[--book_count];
		}
	}
	for (int i = 0; i < count; i++) {
		if (find_locked(found[i].e.id)) {
			free(found[i].files); // queued, running or being deleted: what is in memory is newer
			continue;
		}
		book_t *b = add_locked(found[i].e.id);
		if (b) {
			*b = found[i];
		} else {
			free(found[i].files);
		}
	}
	free(found);
	changed_locked();
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// downloading
// ---------------------------------------------------------------------------

static void set_error(const char *id, const char *error) {
	pthread_mutex_lock(&lock);
	book_t *b = find_locked(id);
	if (b) {
		snprintf(b->e.error, sizeof(b->e.error), "%s", error ? error : "");
		b->e.state = ABSDL_PARTIAL;
		changed_locked();
	}
	pthread_mutex_unlock(&lock);
}

// Counted at every piece, told at most once a second: each telling redraws
// the page watching, and on one core that is time taken from the download.
static void add_progress(const char *id, long long bytes) {
	static struct timespec last_told;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&lock);
	book_t *b = find_locked(id);
	if (b) {
		b->e.done += bytes;
		if (now.tv_sec != last_told.tv_sec) {
			last_told = now;
			changed_locked();
		}
	}
	pthread_mutex_unlock(&lock);
}

typedef enum {
	GET_OK,
	GET_FAILED,
	GET_CANCELLED,
	GET_UNAUTHORISED,
	GET_DISK,
} get_t;

// `url` into `part`, from where `part` already ends, renamed to `final` once it
// holds `expected` bytes (0: whatever the server sends). `id` gets the bytes
// counted against it as they arrive.
static get_t download(const char *url, const char *part, const char *final, long long expected, const char *id,
					  char *error, size_t error_size) {
	long long have = file_size(part);
	if (have < 0) {
		have = 0;
	}
	if (expected > 0 && have > expected) {
		unlink(part); // something else wrote here
		add_progress(id, -have);
		have = 0;
	}
	if (expected > 0 && have == expected) {
		return rename(part, final) == 0 ? GET_OK : GET_DISK;
	}

	char auth[2400];
	if (!abs_auth_header(auth, sizeof(auth))) {
		return GET_UNAUTHORISED;
	}
	http_stream_opts_t opts = {.extra_headers = auth, .offset = have, .no_icy = true};
	http_stream_t st;
	bool opened = http_stream_open_ex(&st, url, &opts, TIMEOUT_SECS);
	memset(auth, 0, sizeof(auth));
	if (!opened) {
		if (st.status == 401 || st.status == 403) {
			return GET_UNAUTHORISED;
		}
		const char *why = http_last_error();
		snprintf(error, error_size, "%s", why && why[0] ? why : "abs_no_reply");
		return GET_FAILED;
	}

	FILE *f;
	if (st.range_start == have && have > 0) {
		f = fopen(part, "ab");
	} else {
		// The server sent the file from its start: what was there goes.
		f = fopen(part, "wb");
		add_progress(id, -have);
		have = 0;
	}
	if (!f) {
		http_stream_close(&st);
		snprintf(error, error_size, "%s", "abs_card_write_failed");
		return GET_DISK;
	}

	char *buf = malloc(CHUNK);
	get_t result = GET_OK;
	for (;;) {
		if (cancel_running) {
			result = GET_CANCELLED;
			break;
		}
		int n = buf ? http_stream_read(&st, buf, CHUNK) : -1;
		if (n == 0) {
			break;
		}
		if (n < 0) {
			const char *why = http_last_error();
			snprintf(error, error_size, "%s", why && why[0] ? why : "abs_download_cut");
			result = GET_FAILED;
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			snprintf(error, error_size, "%s", "abs_card_write_failed");
			result = GET_DISK;
			break;
		}
		have += n;
		add_progress(id, n);
	}
	free(buf);
	http_stream_close(&st);
	if (fclose(f) != 0 && result == GET_OK) {
		snprintf(error, error_size, "%s", "abs_card_write_failed");
		result = GET_DISK;
	}
	if (result != GET_OK) {
		return result;
	}
	if (expected > 0 && have != expected) {
		snprintf(error, error_size, "%s", "abs_download_cut");
		return GET_FAILED;
	}
	return rename(part, final) == 0 ? GET_OK : GET_DISK;
}

// The same, renewing the token once when the server says it ran out.
static get_t download_signed(const char *url, const char *part, const char *final, long long expected, const char *id,
							 char *error, size_t error_size) {
	get_t r = download(url, part, final, expected, id, error, error_size);
	if (r == GET_UNAUTHORISED && abs_renew()) {
		r = download(url, part, final, expected, id, error, error_size);
	}
	if (r == GET_UNAUTHORISED) {
		snprintf(error, error_size, "%s", "abs_expired");
	}
	return r;
}

static void mkdirs(const char *path) {
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

// Whether `folder` is free to put book `id` in: not there yet, or ours already.
static bool folder_usable(const char *folder, const char *id) {
	struct stat st;
	if (stat(folder, &st) != 0) {
		return true;
	}
	book_t b;
	bool complete;
	if (!sidecar_read(folder, &b, &complete)) {
		return false; // somebody's own folder
	}
	bool ours = strcmp(b.e.id, id) == 0;
	free(b.files);
	return ours;
}

// Audiobooks/<author>/<series>/<title>, with " (ABS)" -- then " (ABS 2)"... --
// after the title when that folder already holds someone else's files.
static bool choose_folder(const char *root, const char *id, const char *author, const char *series, const char *title,
						  char *out, size_t size) {
	char a[200], s[200], t[200], base[700];
	int n;
	podcastdl_safe_name(author, "Unknown author", a, sizeof(a));
	podcastdl_safe_name(series, "", s, sizeof(s));
	podcastdl_safe_name(title, id, t, sizeof(t));
	if (s[0]) {
		n = snprintf(base, sizeof(base), "%s/%s/%s/%s/%s", root, AUDIOBOOKDB_FOLDER, a, s, t);
	} else {
		n = snprintf(base, sizeof(base), "%s/%s/%s/%s", root, AUDIOBOOKDB_FOLDER, a, t);
	}
	// Room for " (ABS 9)" too: a cut path would be some other folder.
	if (n < 0 || (size_t)n + 9 >= size) {
		return false;
	}
	memcpy(out, base, (size_t)n + 1);
	for (int k = 1; k < 10; k++) {
		if (k == 2) {
			snprintf(out + n, size - (size_t)n, " (ABS)");
		} else if (k > 2) {
			snprintf(out + n, size - (size_t)n, " (ABS %d)", k - 1);
		}
		if (folder_usable(out, id)) {
			return true;
		}
	}
	return false;
}

static int compare_index(const void *a, const void *b) {
	const int *x = a, *y = b;
	return x[0] - y[0];
}

// What the book page needs from the server to download it: the files in play
// order, the author and series for the folder, and where the user is in it.
typedef struct {
	book_t book;
	char series[200];
	bool has_cover;
	double current_time;
	bool finished;
	bool has_progress;
} plan_t;

static bool read_plan(const char *id, plan_t *plan, char *error, size_t error_size) {
	char path[160];
	snprintf(path, sizeof(path), "/api/items/%s?expanded=1&include=progress", id);
	abs_req_t req = {.path = path, .limit = DETAIL_LIMIT};
	char *body = NULL;
	int status = 0;
	if (!abs_request(&req, &body, NULL, &status) || status != 200) {
		const char *why = http_last_error();
		snprintf(error, error_size, "%s",
				 status == 401 ? "abs_expired" : status == 404 ? "abs_gone" : why && why[0] ? why : "abs_no_reply");
		free(body);
		return false;
	}
	json_doc_t doc;
	bool ok = json_parse(body, &doc);
	if (!ok) {
		snprintf(error, error_size, "%s", "abs_bad_answer");
		free(body);
		return false;
	}
	memset(plan, 0, sizeof(*plan));
	book_t *b = &plan->book;
	snprintf(b->e.id, sizeof(b->e.id), "%s", id);
	int item = json_root(&doc);
	int media = json_get(&doc, item, "media");
	int meta = json_get(&doc, media, "metadata");
	json_obj_str(&doc, meta, "title", b->e.title, sizeof(b->e.title));
	json_obj_str(&doc, meta, "authorName", b->e.author, sizeof(b->e.author));
	json_obj_str(&doc, json_at(&doc, json_get(&doc, meta, "series"), 0), "name", plan->series, sizeof(plan->series));
	int cover = json_get(&doc, media, "coverPath");
	plan->has_cover = cover >= 0 && doc.toks[cover].type == JSON_STRING;

	int files = json_get(&doc, media, "audioFiles");
	int n = json_len(&doc, files);
	int (*order)[2] = n > 0 ? calloc((size_t)n, sizeof(*order)) : NULL; // {index, token}
	b->files = n > 0 ? calloc((size_t)n, sizeof(dlfile_t)) : NULL;
	for (int i = 0; order && i < n; i++) {
		int f = json_at(&doc, files, i);
		order[i][0] = (int)json_obj_long(&doc, f, "index", i);
		order[i][1] = f;
	}
	if (order) {
		qsort(order, (size_t)n, sizeof(*order), compare_index);
	}
	double start = 0;
	for (int i = 0; order && b->files && i < n; i++) {
		int f = order[i][1];
		dlfile_t *d = &b->files[b->file_count++];
		int fm = json_get(&doc, f, "metadata");
		char original[256] = "";
		json_obj_str(&doc, fm, "filename", original, sizeof(original));
		podcastdl_safe_name(original, "part", d->name, sizeof(d->name));
		json_obj_str(&doc, f, "ino", d->ino, sizeof(d->ino));
		d->size = json_obj_llong(&doc, fm, "size", 0);
		d->duration = json_double(&doc, json_get(&doc, f, "duration"), 0);
		d->start = start;
		start += d->duration;
		b->e.total += d->size;
	}
	b->e.duration = start;
	free(order);

	// The parts are played in the order of their names. When the server's
	// names would not come out in its own order, they are numbered.
	bool ordered = true;
	for (int i = 1; i < b->file_count; i++) {
		if (audiobookdb_name_compare(b->files[i - 1].name, b->files[i].name) >= 0) {
			ordered = false;
		}
	}
	for (int i = 0; !ordered && i < b->file_count; i++) {
		char numbered[sizeof(b->files[i].name)];
		snprintf(numbered, sizeof(numbered), "%03d - %.240s", i + 1, b->files[i].name);
		snprintf(b->files[i].name, sizeof(b->files[i].name), "%s", numbered);
	}

	int progress = json_get(&doc, item, "userMediaProgress");
	if (progress >= 0 && doc.toks[progress].type == JSON_OBJECT) {
		plan->has_progress = true;
		plan->current_time = json_double(&doc, json_get(&doc, progress, "currentTime"), 0);
		plan->finished = json_obj_bool(&doc, progress, "isFinished", false);
	}
	json_free(&doc);
	free(body);

	if (b->file_count == 0) {
		snprintf(error, error_size, "%s", "abs_no_audio");
		free(b->files);
		return false;
	}
	return true;
}

static long long card_free(const char *root) {
	struct statvfs st;
	return statvfs(root, &st) == 0 ? (long long)st.f_bavail * (long long)st.f_frsize : -1;
}

// The server's place in the book written into the audiobook index, so the
// player starts the book where it was left on the server.
static void seed_position(const book_t *b, const plan_t *plan) {
	if (!plan->has_progress) {
		return;
	}
	char first[900], key[900];
	snprintf(first, sizeof(first), "%s/%s", b->e.folder, b->files[0].name);
	if (!audiobookdb_book_for_file(first, key, sizeof(key))) {
		return;
	}
	if (plan->finished) {
		audiobookdb_mark_finished(key);
		return;
	}
	for (int i = b->file_count - 1; i >= 0; i--) {
		if (plan->current_time >= b->files[i].start) {
			char file[900];
			snprintf(file, sizeof(file), "%s/%s", b->e.folder, b->files[i].name);
			audiobookdb_save_position(key, file, plan->current_time - b->files[i].start);
			return;
		}
	}
}

static void run_download(const char *id, const char *root) {
	char error[160] = "";
	plan_t plan;
	if (!read_plan(id, &plan, error, sizeof(error))) {
		set_error(id, error);
		return;
	}
	book_t *b = &plan.book;

	// A book started before goes on in its own folder.
	pthread_mutex_lock(&lock);
	book_t *known = find_locked(id);
	if (known && known->e.folder[0]) {
		snprintf(b->e.folder, sizeof(b->e.folder), "%s", known->e.folder);
	}
	pthread_mutex_unlock(&lock);
	if (!b->e.folder[0] && !choose_folder(root, id, b->e.author, plan.series, b->e.title, b->e.folder, sizeof(b->e.folder))) {
		free(b->files);
		set_error(id, "abs_no_folder");
		return;
	}
	mkdirs(b->e.folder);

	long long present = bytes_present(b);
	long long free_bytes = card_free(root);
	if (free_bytes >= 0 && b->e.total - present + SPACE_MARGIN > free_bytes) {
		free(b->files);
		set_error(id, "abs_no_space");
		return;
	}

	char server[256] = "";
	abs_server(server, sizeof(server));
	if (!sidecar_write(b, server, false)) {
		free(b->files);
		set_error(id, "abs_card_write_failed");
		return;
	}

	pthread_mutex_lock(&lock);
	book_t *entry = add_locked(id);
	if (entry) {
		free(entry->files);
		entry->files = NULL;
		entry->file_count = 0;
		snprintf(entry->e.title, sizeof(entry->e.title), "%s", b->e.title);
		snprintf(entry->e.author, sizeof(entry->e.author), "%s", b->e.author);
		snprintf(entry->e.folder, sizeof(entry->e.folder), "%s", b->e.folder);
		entry->e.duration = b->e.duration;
		entry->e.total = b->e.total;
		entry->e.done = present;
		entry->e.error[0] = '\0';
		changed_locked();
	}
	pthread_mutex_unlock(&lock);

	// The cover, beside the files, where the audiobook pages look for it.
	char cover[900];
	snprintf(cover, sizeof(cover), "%s/%s", b->e.folder, COVER_NAME);
	if (plan.has_cover && access(cover, F_OK) != 0) {
		char url[600], part[910];
		snprintf(url, sizeof(url), "%s/api/items/%s/cover?width=%d&format=jpeg", server, id, COVER_SIZE);
		snprintf(part, sizeof(part), "%s/.%s.part", b->e.folder, COVER_NAME);
		char ignored[160];
		// Not counted against the book: its size is not known beforehand.
		if (download_signed(url, part, cover, 0, "", ignored, sizeof(ignored)) != GET_OK) {
			unlink(part);
		}
	}

	get_t result = GET_OK;
	for (int i = 0; i < b->file_count && result == GET_OK; i++) {
		const dlfile_t *d = &b->files[i];
		char final[900], part[910], url[600];
		snprintf(final, sizeof(final), "%s/%s", b->e.folder, d->name);
		if (file_size(final) == d->size) {
			continue; // whole already
		}
		snprintf(part, sizeof(part), "%s/.%s.part", b->e.folder, d->name);
		snprintf(url, sizeof(url), "%s/api/items/%s/file/%s/download", server, id, d->ino);
		result = download_signed(url, part, final, d->size, id, error, sizeof(error));
	}

	if (result != GET_OK) {
		free(b->files);
		set_error(id, result == GET_CANCELLED ? "" : error);
		return;
	}

	sidecar_write(b, server, true);

	char shelf[600];
	audiobooks_root(root, shelf, sizeof(shelf));
	audiobookdb_scan_folder(shelf, b->e.folder);
	seed_position(b, &plan);

	pthread_mutex_lock(&lock);
	entry = find_locked(id);
	if (entry) {
		free(entry->files);
		entry->files = b->files;
		entry->file_count = b->file_count;
		b->files = NULL;
		entry->e.state = ABSDL_DONE;
		entry->e.done = entry->e.total;
		changed_locked();
	}
	pthread_mutex_unlock(&lock);
	free(b->files);
}

// Only what this module wrote: the files the .abs.json lists, their .part
// halves, the cover and the .abs.json itself. Then the folder, and the author
// and series folders above it, when that leaves them empty.
static void run_delete(const char *id, const char *root) {
	book_t b = {0};
	char folder[512] = "";
	pthread_mutex_lock(&lock);
	book_t *known = find_locked(id);
	if (known) {
		snprintf(folder, sizeof(folder), "%s", known->e.folder);
	}
	pthread_mutex_unlock(&lock);

	bool complete;
	bool read = folder[0] && sidecar_read(folder, &b, &complete);
	// Another book's .abs.json: its files are not this book's to delete.
	if (read && strcmp(b.e.id, id) != 0) {
		free(b.files);
		read = false;
	}
	if (read) {
		for (int i = 0; i < b.file_count; i++) {
			char path[900];
			snprintf(path, sizeof(path), "%s/%s", b.e.folder, b.files[i].name);
			unlink(path);
			snprintf(path, sizeof(path), "%s/.%s.part", b.e.folder, b.files[i].name);
			unlink(path);
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", b.e.folder, COVER_NAME);
		unlink(path);
		snprintf(path, sizeof(path), "%s/.%s.part", b.e.folder, COVER_NAME);
		unlink(path);
		snprintf(path, sizeof(path), "%s/%s", b.e.folder, SIDECAR);
		unlink(path);

		char shelf[600];
		audiobooks_root(root, shelf, sizeof(shelf));
		char dir[700];
		snprintf(dir, sizeof(dir), "%s", b.e.folder);
		size_t shelf_len = strlen(shelf);
		while (strlen(dir) > shelf_len && rmdir(dir) == 0) {
			char *slash = strrchr(dir, '/');
			if (!slash) {
				break;
			}
			*slash = '\0';
		}
		audiobookdb_scan_folder(shelf, b.e.folder);
		free(b.files);
	}

	pthread_mutex_lock(&lock);
	remove_locked(id);
	changed_locked();
	pthread_mutex_unlock(&lock);
}

static void *worker(void *arg) {
	(void)arg;
	// Low, not idle: somebody asked for this book and is waiting for it, as
	// with a podcast episode being saved. Idle starves it whenever the
	// interface is busy, and the TLS underneath wants the CPU.
	thread_be_low_priority("abs downloads");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!walk_needed && queue_len == 0 && deletion_count == 0) {
			pthread_cond_wait(&wake, &lock);
		}
		bool do_walk = walk_needed;
		walk_needed = false;
		char id[40] = "";
		bool deleting = false;
		if (!do_walk && deletion_count > 0) {
			snprintf(id, sizeof(id), "%s", deletions[0]);
			memmove(deletions, deletions + 1, (size_t)(--deletion_count) * sizeof(deletions[0]));
			deleting = true;
		} else if (!do_walk && queue_len > 0) {
			snprintf(id, sizeof(id), "%s", queue[0]);
			memmove(queue, queue + 1, (size_t)(--queue_len) * sizeof(queue[0]));
			book_t *b = find_locked(id);
			if (b) {
				b->e.state = ABSDL_RUNNING;
			}
			snprintf(running, sizeof(running), "%s", id);
			cancel_running = false;
			changed_locked();
		}
		char root[512];
		snprintf(root, sizeof(root), "%s", card_root);
		pthread_mutex_unlock(&lock);

		if (do_walk) {
			refresh_from_card();
		} else if (deleting) {
			run_delete(id, root);
		} else if (id[0] && root[0]) {
			run_download(id, root);
			pthread_mutex_lock(&lock);
			running[0] = '\0';
			changed_locked();
			pthread_mutex_unlock(&lock);
		}
	}
	return NULL;
}

static void start_locked(void) {
	if (!started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker, NULL) == 0) {
			pthread_detach(thread);
			started = true;
		}
	}
	pthread_cond_signal(&wake);
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void absdl_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	walk_needed = true;
	start_locked();
	pthread_mutex_unlock(&lock);
}

void absdl_refresh(void) {
	pthread_mutex_lock(&lock);
	if (card_root[0]) {
		walk_needed = true;
		start_locked();
	}
	pthread_mutex_unlock(&lock);
}

bool absdl_enqueue(const char *item_id, const char *title, const char *author) {
	if (!item_id || !item_id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	book_t *b = find_locked(item_id);
	bool ok = queue_len < ABSDL_QUEUE_MAX &&
			  !(b && (b->e.state == ABSDL_QUEUED || b->e.state == ABSDL_RUNNING || b->e.state == ABSDL_DONE ||
					  b->e.state == ABSDL_DELETING));
	if (ok) {
		b = add_locked(item_id);
		ok = b != NULL;
	}
	if (ok) {
		if (!b->e.title[0]) {
			snprintf(b->e.title, sizeof(b->e.title), "%s", title ? title : "");
			snprintf(b->e.author, sizeof(b->e.author), "%s", author ? author : "");
		}
		b->e.state = ABSDL_QUEUED;
		b->e.error[0] = '\0';
		snprintf(queue[queue_len++], sizeof(queue[0]), "%s", item_id);
		changed_locked();
		start_locked();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

void absdl_cancel(const char *item_id) {
	pthread_mutex_lock(&lock);
	if (strcmp(running, item_id) == 0) {
		cancel_running = true;
	}
	for (int i = 0; i < queue_len; i++) {
		if (strcmp(queue[i], item_id) == 0) {
			memmove(queue + i, queue + i + 1, (size_t)(queue_len - i - 1) * sizeof(queue[0]));
			queue_len--;
			break;
		}
	}
	book_t *b = find_locked(item_id);
	if (b && b->e.state == ABSDL_QUEUED) {
		if (b->e.folder[0]) {
			b->e.state = ABSDL_PARTIAL;
		} else {
			remove_locked(item_id); // never started: nothing on the card
		}
	}
	changed_locked();
	pthread_mutex_unlock(&lock);
}

bool absdl_delete(const char *item_id) {
	absdl_cancel(item_id);
	pthread_mutex_lock(&lock);
	book_t *b = find_locked(item_id);
	bool ok = b && deletion_count < ABSDL_QUEUE_MAX && b->e.state != ABSDL_DELETING;
	if (ok) {
		b->e.state = ABSDL_DELETING;
		snprintf(deletions[deletion_count++], sizeof(deletions[0]), "%s", item_id);
		changed_locked();
		start_locked();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool absdl_get(const char *item_id, absdl_entry_t *out) {
	pthread_mutex_lock(&lock);
	book_t *b = item_id ? find_locked(item_id) : NULL;
	if (b) {
		*out = b->e;
	}
	pthread_mutex_unlock(&lock);
	return b != NULL;
}

static int state_rank(absdl_state_t s) {
	return s == ABSDL_RUNNING ? 0 : s == ABSDL_QUEUED ? 1 : 2;
}

static int compare_entries(const void *a, const void *b) {
	const absdl_entry_t *x = a, *y = b;
	int rx = state_rank(x->state), ry = state_rank(y->state);
	if (rx != ry) {
		return rx - ry;
	}
	return strcasecmp(x->title, y->title);
}

int absdl_list(absdl_entry_t *out, int max) {
	pthread_mutex_lock(&lock);
	int n = 0;
	for (int i = 0; i < book_count && n < max; i++) {
		out[n++] = books[i].e;
	}
	pthread_mutex_unlock(&lock);
	if (n > 1) {
		qsort(out, (size_t)n, sizeof(*out), compare_entries);
	}
	return n;
}

unsigned absdl_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned s = serial;
	pthread_mutex_unlock(&lock);
	return s;
}

bool absdl_item_for_file(const char *path, char *id_out, size_t id_size, double seconds, double *book_seconds) {
	if (!path) {
		return false;
	}
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < book_count && !found; i++) {
		const book_t *b = &books[i];
		size_t len = strlen(b->e.folder);
		if (!len || strncmp(path, b->e.folder, len) != 0 || path[len] != '/') {
			continue;
		}
		for (int k = 0; k < b->file_count; k++) {
			if (strcmp(path + len + 1, b->files[k].name) == 0) {
				snprintf(id_out, id_size, "%s", b->e.id);
				if (book_seconds) {
					*book_seconds = b->files[k].start + seconds;
				}
				found = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

bool absdl_file_at(const char *item_id, double book_seconds, char *file_out, size_t file_size_out, double *seconds_out) {
	bool found = false;
	pthread_mutex_lock(&lock);
	const book_t *b = find_locked(item_id);
	if (b && b->e.state == ABSDL_DONE && b->file_count > 0) {
		int k = 0;
		for (int i = b->file_count - 1; i >= 0; i--) {
			if (book_seconds >= b->files[i].start) {
				k = i;
				break;
			}
		}
		snprintf(file_out, file_size_out, "%s/%s", b->e.folder, b->files[k].name);
		*seconds_out = book_seconds > b->files[k].start ? book_seconds - b->files[k].start : 0;
		found = true;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

bool absdl_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = running[0] != '\0';
	pthread_mutex_unlock(&lock);
	return busy;
}
