#define _GNU_SOURCE 1

#include "abscatalog.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/abs/abs.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/core/utils.h"
#include "src/system/net/http.h"

#define CACHE_DIR ".local/abs/cache"

// Books are read a hundred at a time: a page is about 170 KB of JSON, and its
// tokens fit comfortably beside it.
#define PAGE_SIZE 100
#define NAMES_PAGE_SIZE 100
#define CONTINUE_LIMIT 50
#define SEARCH_LIMIT 25
#define SEARCH_SERIES_MAX 3 // series whose books a search lists
#define BODY_LIMIT (4 * 1024 * 1024)

typedef enum {
	JOB_NONE,
	JOB_LIBRARIES,
	JOB_BOOKS,
	JOB_NAMES,
	JOB_DETAIL,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char library[ABS_ID_MAX];
	abs_list_t list;
	abs_filter_t filter;
	char filter_id[ABS_ID_MAX];
	abs_sort_t sort;
	char query[128];
	abs_names_t names;
	char item[ABS_ID_MAX];
} job_t;

// What the worker built, waiting for the GUI to take it.
typedef struct {
	abs_book_t *books;
	int book_count;
	abs_name_t *names;
	int name_count;
	abs_library_t libraries[ABS_MAX_LIBRARIES];
	int library_count;
	abs_detail_t detail;
	bool has_detail;
} rows_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

static job_t job;
static bool job_waiting;
static unsigned job_generation; // bumped by every request
static bool working;

static abs_result_t pending_result;
static rows_t pending_rows;
static bool pending_ready;
static unsigned serial;

static char cache_root[512];

// The GUI's side: only ever touched on its thread.
static abs_result_t front_result;
static rows_t front_rows;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void rows_free(rows_t *r) {
	free(r->books);
	free(r->names);
	free(r->detail.description);
	memset(r, 0, sizeof(*r));
}

static bool job_current(unsigned generation) {
	pthread_mutex_lock(&lock);
	bool current = generation == job_generation;
	pthread_mutex_unlock(&lock);
	return current;
}

static void set_progress(unsigned generation, int loaded, int total) {
	pthread_mutex_lock(&lock);
	if (generation == job_generation) {
		pending_result.state = ABS_RESULT_LOADING;
		pending_result.loaded = loaded;
		pending_result.total = total;
	}
	pthread_mutex_unlock(&lock);
}

static void base64_encode(const char *in, char *out, size_t size) {
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t len = strlen(in), o = 0;
	const unsigned char *p = (const unsigned char *)in;
	for (size_t i = 0; i < len && o + 5 < size; i += 3) {
		unsigned v = (unsigned)p[i] << 16 | (i + 1 < len ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
		out[o++] = T[(v >> 18) & 63];
		out[o++] = T[(v >> 12) & 63];
		out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? T[v & 63] : '=';
	}
	out[o] = '\0';
}

// "He Who Fights with Monsters #10" -> 10. The first series when there are
// several ("A #1, B #4").
static double series_sequence(const char *series) {
	const char *hash = strchr(series, '#');
	return hash ? strtod(hash + 1, NULL) : 0.0;
}

// The text of an HTML description: tags dropped, paragraphs and line breaks
// kept as new lines, the common entities decoded.
static char *strip_html(const char *in) {
	size_t len = strlen(in);
	char *out = malloc(len + 1);
	if (!out) {
		return NULL;
	}
	size_t o = 0;
	for (size_t i = 0; i < len;) {
		if (in[i] == '<') {
			const char *end = strchr(in + i, '>');
			if (!end) {
				break;
			}
			bool breaks = strncasecmp(in + i, "<br", 3) == 0 || strncasecmp(in + i, "</p", 3) == 0 ||
						  strncasecmp(in + i, "</div", 5) == 0 || strncasecmp(in + i, "</li", 4) == 0;
			if (breaks && o > 0 && out[o - 1] != '\n') {
				out[o++] = '\n';
			}
			i = (size_t)(end - in) + 1;
			continue;
		}
		if (in[i] == '&') {
			static const struct {
				const char *entity;
				char c;
			} E[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}};
			bool done = false;
			for (size_t k = 0; k < sizeof(E) / sizeof(E[0]); k++) {
				size_t n = strlen(E[k].entity);
				if (strncmp(in + i, E[k].entity, n) == 0) {
					out[o++] = E[k].c;
					i += n;
					done = true;
					break;
				}
			}
			if (done) {
				continue;
			}
		}
		out[o++] = in[i++];
	}
	while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) {
		o--;
	}
	out[o] = '\0';
	return out;
}

// ---------------------------------------------------------------------------
// the copies kept on the card
// ---------------------------------------------------------------------------

static void cache_path(const char *root, const char *request, char *out, size_t size) {
	char hex[MD5_HEX_LEN];
	char server[256] = "";
	abs_server(server, sizeof(server));
	char key[2600];
	snprintf(key, sizeof(key), "%s%s", server, request);
	md5_hex(key, strlen(key), hex);
	snprintf(out, size, "%s/%s/%s.json", root, CACHE_DIR, hex);
}

static void mkdirs(const char *path) {
	char tmp[600];
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

static void cache_write(const char *root, const char *request, const char *body) {
	if (!root[0]) {
		return;
	}
	char dir[600], path[700], tmp[710];
	snprintf(dir, sizeof(dir), "%s/%s", root, CACHE_DIR);
	mkdirs(dir);
	cache_path(root, request, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	bool ok = fputs(body, f) >= 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
	}
}

static char *cache_read(const char *root, const char *request) {
	if (!root[0]) {
		return NULL;
	}
	char path[700];
	cache_path(root, request, path, sizeof(path));
	return read_file_content(path);
}

// The answer to `request`: from the server when it answers, else the copy kept
// last time, with *offline set. NULL with `error` filled when there is
// neither.
static char *fetch(const char *root, const char *request, bool *offline, char *error, size_t error_size) {
	char *body = NULL;
	int status = 0;
	abs_req_t req = {.path = request, .limit = BODY_LIMIT};
	bool answered = abs_request(&req, &body, NULL, &status);
	if (answered && status == 200 && body) {
		cache_write(root, request, body);
		return body;
	}
	free(body);

	char *kept = cache_read(root, request);
	if (kept) {
		*offline = true;
		return kept;
	}
	abs_status_t st;
	abs_get_status(&st);
	if (st.state == ABS_STATE_EXPIRED) {
		snprintf(error, error_size, "%s", "abs_expired");
	} else if (answered) {
		snprintf(error, error_size, "HTTP %d", status);
	} else {
		const char *why = http_last_error();
		snprintf(error, error_size, "%s", why && why[0] ? why : "abs_no_reply");
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

static void parse_book(const json_doc_t *doc, int item, abs_book_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, item, "id", out->id, sizeof(out->id));
	out->added = json_obj_llong(doc, item, "addedAt", 0);
	out->size = json_obj_llong(doc, item, "size", 0);
	int media = json_get(doc, item, "media");
	out->duration = json_double(doc, json_get(doc, media, "duration"), 0);
	int cover = json_get(doc, media, "coverPath");
	out->has_cover = cover >= 0 && doc->toks[cover].type == JSON_STRING;
	int meta = json_get(doc, media, "metadata");
	json_obj_str(doc, meta, "title", out->title, sizeof(out->title));
	json_obj_str(doc, meta, "authorName", out->author, sizeof(out->author));
	json_obj_str(doc, meta, "seriesName", out->series, sizeof(out->series));
	out->sequence = series_sequence(out->series);
}

typedef struct {
	char id[ABS_ID_MAX];
	double progress;
	double current_time;
	bool finished;
} progress_t;

typedef struct {
	progress_t *items;
	int count;
} progress_list_t;

// Every book the user started, from GET /api/me.
static void read_progress(const char *root, progress_list_t *out, bool *offline) {
	memset(out, 0, sizeof(*out));
	char error[160];
	char *body = fetch(root, "/api/me", offline, error, sizeof(error));
	if (!body) {
		return;
	}
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int list = json_get(&doc, json_root(&doc), "mediaProgress");
		int n = json_len(&doc, list);
		out->items = n > 0 ? calloc((size_t)n, sizeof(progress_t)) : NULL;
		for (int i = 0; out->items && i < n; i++) {
			int p = json_at(&doc, list, i);
			progress_t *dst = &out->items[out->count];
			json_obj_str(&doc, p, "libraryItemId", dst->id, sizeof(dst->id));
			// Podcast episodes have progress too; this player lists books.
			int episode = json_get(&doc, p, "episodeId");
			if (!dst->id[0] || (episode >= 0 && doc.toks[episode].type == JSON_STRING)) {
				continue;
			}
			dst->progress = json_double(&doc, json_get(&doc, p, "progress"), 0);
			dst->current_time = json_double(&doc, json_get(&doc, p, "currentTime"), 0);
			dst->finished = json_obj_bool(&doc, p, "isFinished", false);
			out->count++;
		}
		json_free(&doc);
	}
	free(body);
}

static void apply_progress(abs_book_t *books, int count, const progress_list_t *progress) {
	for (int i = 0; i < count; i++) {
		for (int k = 0; k < progress->count; k++) {
			if (strcmp(books[i].id, progress->items[k].id) == 0) {
				// The position, not the server's own fraction: a client that
				// updates one and not the other leaves the fraction behind.
				const progress_t *p = &progress->items[k];
				books[i].progress = p->current_time > 0 && books[i].duration > 0 ? p->current_time / books[i].duration
																				 : p->progress;
				books[i].finished = p->finished;
				books[i].started = true;
				break;
			}
		}
	}
}

static bool books_push(rows_t *r, int *capacity, const abs_book_t *book) {
	// The same book twice -- a search that finds it by title and in its series.
	for (int i = 0; i < r->book_count; i++) {
		if (strcmp(r->books[i].id, book->id) == 0) {
			return true;
		}
	}
	if (r->book_count == *capacity) {
		int grown = *capacity ? *capacity * 2 : 64;
		abs_book_t *more = realloc(r->books, (size_t)grown * sizeof(*more));
		if (!more) {
			return false;
		}
		r->books = more;
		*capacity = grown;
	}
	r->books[r->book_count++] = *book;
	return true;
}

static int compare_series(const void *a, const void *b) {
	const abs_book_t *x = a, *y = b;
	if (x->sequence != y->sequence) {
		// Books with no number go after the numbered ones.
		if (x->sequence == 0) {
			return 1;
		}
		if (y->sequence == 0) {
			return -1;
		}
		return x->sequence < y->sequence ? -1 : 1;
	}
	return strcasecmp(x->title, y->title);
}

static int compare_names(const void *a, const void *b) {
	return strcasecmp(((const abs_name_t *)a)->name, ((const abs_name_t *)b)->name);
}

// ---------------------------------------------------------------------------
// the jobs
// ---------------------------------------------------------------------------

// The books of `library`, all pages of them, appended to `r`: the whole
// library, or one author's or one series' books.
static bool fetch_library_books(const char *root, const char *library, abs_filter_t filter_kind, const char *filter_id,
								abs_sort_t order, unsigned generation, rows_t *r, int *capacity, abs_result_t *res) {
	char request[1024];
	const char *sort = order == ABS_SORT_AUTHOR  ? "media.metadata.authorName"
					   : order == ABS_SORT_ADDED ? "addedAt&desc=1"
												 : "media.metadata.title";
	char filter[200] = "";
	if (filter_kind != ABS_FILTER_NONE && filter_id[0]) {
		char b64[96], enc[160];
		base64_encode(filter_id, b64, sizeof(b64));
		http_url_encode(b64, enc, sizeof(enc));
		snprintf(filter, sizeof(filter), "&filter=%s.%s", filter_kind == ABS_FILTER_AUTHOR ? "authors" : "series", enc);
	}
	int first = r->book_count;
	int total = -1;
	for (int page = 0; total < 0 || r->book_count - first < total; page++) {
		if (!job_current(generation)) {
			return false;
		}
		snprintf(request, sizeof(request), "/api/libraries/%s/items?limit=%d&page=%d&minified=1&sort=%s%s", library,
				 PAGE_SIZE, page, sort, filter);
		bool offline = false;
		char *body = fetch(root, request, &offline, res->error, sizeof(res->error));
		if (!body) {
			// The first page missing is a failure; a later one ends the list
			// where it is (offline, with only the first pages kept).
			if (page == 0) {
				return false;
			}
			break;
		}
		res->offline |= offline;
		int got = 0;
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int top = json_root(&doc);
			total = (int)json_obj_long(&doc, top, "total", 0);
			int list = json_get(&doc, top, "results");
			got = json_len(&doc, list);
			for (int i = 0; i < got; i++) {
				abs_book_t book;
				parse_book(&doc, json_at(&doc, list, i), &book);
				books_push(r, capacity, &book);
			}
			json_free(&doc);
		}
		free(body);
		if (got == 0) {
			break;
		}
		set_progress(generation, r->book_count, first + total);
	}
	if (order == ABS_SORT_SERIES && r->book_count - first > 1) {
		qsort(r->books + first, (size_t)(r->book_count - first), sizeof(*r->books), compare_series);
	}
	return true;
}

static bool do_libraries(const char *root, rows_t *r, abs_result_t *res) {
	char *body = fetch(root, "/api/libraries", &res->offline, res->error, sizeof(res->error));
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int list = json_get(&doc, json_root(&doc), "libraries");
		for (int i = 0; i < json_len(&doc, list) && r->library_count < ABS_MAX_LIBRARIES; i++) {
			int lib = json_at(&doc, list, i);
			char type[16] = "";
			json_obj_str(&doc, lib, "mediaType", type, sizeof(type));
			if (strcmp(type, "book") != 0) {
				continue; // podcast libraries: not in this player yet
			}
			abs_library_t *dst = &r->libraries[r->library_count++];
			json_obj_str(&doc, lib, "id", dst->id, sizeof(dst->id));
			json_obj_str(&doc, lib, "name", dst->name, sizeof(dst->name));
		}
		json_free(&doc);
	}
	free(body);
	return true;
}

static bool do_books(const char *root, const job_t *j, unsigned generation, rows_t *r, abs_result_t *res) {
	int capacity = 0;
	char request[1024];

	if (j->list == ABS_LIST_CONTINUE) {
		snprintf(request, sizeof(request), "/api/me/items-in-progress?limit=%d", CONTINUE_LIMIT);
		char *body = fetch(root, request, &res->offline, res->error, sizeof(res->error));
		if (!body) {
			return false;
		}
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int list = json_get(&doc, json_root(&doc), "libraryItems");
			for (int i = 0; i < json_len(&doc, list); i++) {
				int item = json_at(&doc, list, i);
				char type[16] = "";
				json_obj_str(&doc, item, "mediaType", type, sizeof(type));
				if (strcmp(type, "book") != 0) {
					continue;
				}
				abs_book_t book;
				parse_book(&doc, item, &book);
				books_push(r, &capacity, &book);
			}
			json_free(&doc);
		}
		free(body);
	} else if (j->list == ABS_LIST_SEARCH) {
		char q[400];
		char series_ids[SEARCH_SERIES_MAX][ABS_ID_MAX];
		int series_count = 0;
		http_url_encode(j->query, q, sizeof(q));
		snprintf(request, sizeof(request), "/api/libraries/%s/search?q=%s&limit=%d", j->library, q, SEARCH_LIMIT);
		char *body = fetch(root, request, &res->offline, res->error, sizeof(res->error));
		if (!body) {
			return false;
		}
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int top = json_root(&doc);
			int found = json_get(&doc, top, "book");
			for (int i = 0; i < json_len(&doc, found); i++) {
				abs_book_t book;
				parse_book(&doc, json_get(&doc, json_at(&doc, found, i), "libraryItem"), &book);
				books_push(r, &capacity, &book);
			}
			// The books a series hit carries are bare records -- no author, no
			// length -- so its books are asked for as a series list instead.
			int series = json_get(&doc, top, "series");
			for (int i = 0; i < json_len(&doc, series) && series_count < SEARCH_SERIES_MAX; i++) {
				json_obj_str(&doc, json_get(&doc, json_at(&doc, series, i), "series"), "id", series_ids[series_count],
							 ABS_ID_MAX);
				if (series_ids[series_count][0]) {
					series_count++;
				}
			}
			json_free(&doc);
		}
		free(body);
		for (int i = 0; i < series_count; i++) {
			if (!fetch_library_books(root, j->library, ABS_FILTER_SERIES, series_ids[i], ABS_SORT_SERIES, generation, r,
									 &capacity, res) &&
				!job_current(generation)) {
				return false;
			}
		}
	} else if (!fetch_library_books(root, j->library, j->filter, j->filter_id, j->sort, generation, r, &capacity, res)) {
		return false;
	}

	progress_list_t progress;
	bool progress_offline = false;
	read_progress(root, &progress, &progress_offline);
	apply_progress(r->books, r->book_count, &progress);
	free(progress.items);
	return true;
}

static bool do_names(const char *root, const job_t *j, unsigned generation, rows_t *r, abs_result_t *res) {
	char request[512];
	int capacity = 0;

	if (j->names == ABS_NAMES_AUTHORS) {
		snprintf(request, sizeof(request), "/api/libraries/%s/authors", j->library);
		char *body = fetch(root, request, &res->offline, res->error, sizeof(res->error));
		if (!body) {
			return false;
		}
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int list = json_get(&doc, json_root(&doc), "authors");
			int n = json_len(&doc, list);
			r->names = n > 0 ? calloc((size_t)n, sizeof(*r->names)) : NULL;
			for (int i = 0; r->names && i < n; i++) {
				int a = json_at(&doc, list, i);
				abs_name_t *dst = &r->names[r->name_count++];
				json_obj_str(&doc, a, "id", dst->id, sizeof(dst->id));
				json_obj_str(&doc, a, "name", dst->name, sizeof(dst->name));
				dst->books = (int)json_obj_long(&doc, a, "numBooks", 0);
			}
			json_free(&doc);
		}
		free(body);
		if (r->name_count > 1) {
			qsort(r->names, (size_t)r->name_count, sizeof(*r->names), compare_names);
		}
		return true;
	}

	int total = -1;
	for (int page = 0; total < 0 || r->name_count < total; page++) {
		if (!job_current(generation)) {
			return false;
		}
		snprintf(request, sizeof(request), "/api/libraries/%s/series?limit=%d&page=%d&minified=1&sort=name", j->library,
				 NAMES_PAGE_SIZE, page);
		bool offline = false;
		char *body = fetch(root, request, &offline, res->error, sizeof(res->error));
		if (!body) {
			if (page == 0) {
				return false;
			}
			break;
		}
		res->offline |= offline;
		int got = 0;
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			int top = json_root(&doc);
			total = (int)json_obj_long(&doc, top, "total", 0);
			int list = json_get(&doc, top, "results");
			got = json_len(&doc, list);
			for (int i = 0; i < got; i++) {
				if (r->name_count == capacity) {
					int grown = capacity ? capacity * 2 : 64;
					abs_name_t *more = realloc(r->names, (size_t)grown * sizeof(*more));
					if (!more) {
						break;
					}
					r->names = more;
					capacity = grown;
				}
				int s = json_at(&doc, list, i);
				abs_name_t *dst = &r->names[r->name_count++];
				memset(dst, 0, sizeof(*dst));
				json_obj_str(&doc, s, "id", dst->id, sizeof(dst->id));
				json_obj_str(&doc, s, "name", dst->name, sizeof(dst->name));
				dst->books = json_len(&doc, json_get(&doc, s, "books"));
			}
			json_free(&doc);
		}
		free(body);
		if (got == 0) {
			break;
		}
		set_progress(generation, r->name_count, total);
	}
	return true;
}

static bool do_detail(const char *root, const job_t *j, rows_t *r, abs_result_t *res) {
	char request[256];
	snprintf(request, sizeof(request), "/api/items/%s?expanded=1&include=progress", j->item);
	char *body = fetch(root, request, &res->offline, res->error, sizeof(res->error));
	if (!body) {
		return false;
	}
	json_doc_t doc;
	bool ok = json_parse(body, &doc);
	if (ok) {
		int item = json_root(&doc);
		abs_detail_t *d = &r->detail;
		parse_book(&doc, item, &d->book);
		int media = json_get(&doc, item, "media");
		int meta = json_get(&doc, media, "metadata");
		json_obj_str(&doc, meta, "narratorName", d->narrator, sizeof(d->narrator));
		json_obj_str(&doc, meta, "publishedYear", d->published, sizeof(d->published));
		int desc = json_get(&doc, meta, "description");
		if (desc >= 0 && doc.toks[desc].type == JSON_STRING) {
			size_t len = (size_t)(doc.toks[desc].end - doc.toks[desc].start) + 1;
			char *raw = malloc(len * 4 + 1); // escapes only shrink; \u can grow to 3 UTF-8 bytes from 6
			if (raw && json_str(&doc, desc, raw, len * 4 + 1)) {
				d->description = strip_html(raw);
			}
			free(raw);
		}
		d->chapters = json_len(&doc, json_get(&doc, media, "chapters"));
		d->files = json_len(&doc, json_get(&doc, media, "audioFiles"));
		int progress = json_get(&doc, item, "userMediaProgress");
		if (progress >= 0 && doc.toks[progress].type == JSON_OBJECT) {
			d->book.progress = json_double(&doc, json_get(&doc, progress, "progress"), 0);
			d->book.finished = json_obj_bool(&doc, progress, "isFinished", false);
			d->book.started = true;
			d->current_time = json_double(&doc, json_get(&doc, progress, "currentTime"), 0);
			if (d->current_time > 0 && d->book.duration > 0) {
				d->book.progress = d->current_time / d->book.duration;
			}
		}
		r->has_detail = true;
		json_free(&doc);
	} else {
		snprintf(res->error, sizeof(res->error), "%s", "abs_bad_answer");
	}
	free(body);
	return ok;
}

static void *worker(void *arg) {
	(void)arg;
	thread_be_background("abs catalog");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!job_waiting) {
			pthread_cond_wait(&wake, &lock);
		}
		job_t j = job;
		unsigned generation = job_generation;
		job_waiting = false;
		working = true;
		char root[512];
		snprintf(root, sizeof(root), "%s", cache_root);
		pthread_mutex_unlock(&lock);

		rows_t r = {0};
		abs_result_t res = {.state = ABS_RESULT_READY};
		bool ok = false;
		switch (j.kind) {
		case JOB_LIBRARIES:
			ok = do_libraries(root, &r, &res);
			break;
		case JOB_BOOKS:
			ok = do_books(root, &j, generation, &r, &res);
			break;
		case JOB_NAMES:
			ok = do_names(root, &j, generation, &r, &res);
			break;
		case JOB_DETAIL:
			ok = do_detail(root, &j, &r, &res);
			break;
		case JOB_NONE:
			break;
		}
		if (!ok) {
			res.state = ABS_RESULT_FAILED;
		}
		res.loaded = res.total = r.book_count ? r.book_count : r.name_count;

		pthread_mutex_lock(&lock);
		working = false;
		if (generation == job_generation) {
			rows_free(&pending_rows);
			pending_rows = r;
			pending_result = res;
			pending_ready = true;
			serial++;
		} else {
			rows_free(&r); // a newer request is waiting; this answer is not wanted
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static void submit(const job_t *j) {
	pthread_mutex_lock(&lock);
	if (!started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker, NULL) == 0) {
			pthread_detach(thread);
			started = true;
		}
	}
	job = *j;
	job_waiting = true;
	job_generation++;
	pending_ready = false;
	memset(&pending_result, 0, sizeof(pending_result));
	pending_result.state = ABS_RESULT_LOADING;
	serial++;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void abscatalog_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	snprintf(cache_root, sizeof(cache_root), "%s", sd_root ? sd_root : "");
	pthread_mutex_unlock(&lock);
}

void abscatalog_library(char *id_out, size_t id_size, char *name_out, size_t name_size) {
	if (id_out) {
		snprintf(id_out, id_size, "%s", config_get("audiobookshelf", "library_id", ""));
	}
	if (name_out) {
		snprintf(name_out, name_size, "%s", config_get("audiobookshelf", "library_name", ""));
	}
}

void abscatalog_set_library(const char *id, const char *name) {
	config_set("audiobookshelf", "library_id", id ? id : "");
	config_set("audiobookshelf", "library_name", name ? name : "");
	config_save();
}

void abscatalog_request_libraries(void) {
	job_t j = {.kind = JOB_LIBRARIES};
	submit(&j);
}

void abscatalog_request_books(abs_list_t list, abs_filter_t filter, const char *filter_id, abs_sort_t sort,
							  const char *query) {
	job_t j = {.kind = JOB_BOOKS, .list = list, .filter = filter, .sort = sort};
	abscatalog_library(j.library, sizeof(j.library), NULL, 0);
	snprintf(j.filter_id, sizeof(j.filter_id), "%s", filter_id ? filter_id : "");
	snprintf(j.query, sizeof(j.query), "%s", query ? query : "");
	submit(&j);
}

void abscatalog_request_names(abs_names_t kind) {
	job_t j = {.kind = JOB_NAMES, .names = kind};
	abscatalog_library(j.library, sizeof(j.library), NULL, 0);
	submit(&j);
}

void abscatalog_request_detail(const char *item_id) {
	job_t j = {.kind = JOB_DETAIL};
	snprintf(j.item, sizeof(j.item), "%s", item_id ? item_id : "");
	submit(&j);
}

unsigned abscatalog_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned s = serial;
	pthread_mutex_unlock(&lock);
	return s;
}

void abscatalog_result(abs_result_t *out) {
	pthread_mutex_lock(&lock);
	if (pending_ready) {
		// The rows change hands here, on the GUI thread, so they never change
		// under a page that is drawing them.
		rows_free(&front_rows);
		front_rows = pending_rows;
		memset(&pending_rows, 0, sizeof(pending_rows));
		front_result = pending_result;
		pending_ready = false;
	} else if (pending_result.state == ABS_RESULT_LOADING) {
		front_result = pending_result;
	}
	*out = front_result;
	pthread_mutex_unlock(&lock);
}

const abs_book_t *abscatalog_books(int *count) {
	*count = front_rows.book_count;
	return front_rows.books;
}

const abs_name_t *abscatalog_names(int *count) {
	*count = front_rows.name_count;
	return front_rows.names;
}

const abs_library_t *abscatalog_libraries(int *count) {
	*count = front_rows.library_count;
	return front_rows.libraries;
}

const abs_detail_t *abscatalog_detail(void) { return front_rows.has_detail ? &front_rows.detail : NULL; }

void abscatalog_cover_url(const char *item_id, int size, char *out, size_t out_size) {
	char server[256];
	if (!abs_server(server, sizeof(server)) || !item_id || !item_id[0]) {
		snprintf(out, out_size, "%s", "");
		return;
	}
	snprintf(out, out_size, "%s/api/items/%s/cover?width=%d&format=jpeg", server, item_id, size);
}

void abscatalog_clear(void) {
	char dir[600];
	pthread_mutex_lock(&lock);
	snprintf(dir, sizeof(dir), "%s/%s", cache_root, CACHE_DIR);
	pthread_mutex_unlock(&lock);
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
}

bool abscatalog_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = working || job_waiting;
	pthread_mutex_unlock(&lock);
	return busy;
}
