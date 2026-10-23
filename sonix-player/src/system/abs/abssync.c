#define _GNU_SOURCE 1

#include "abssync.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/abs/abs.h"
#include "src/system/abs/absdl.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/utils.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define QUEUE_FILE ".local/abs/sessions"
#define OPEN_FILE ".local/abs/sessions.open"
#define STATE_FILE ".local/abs/sync-state"

// Before this the clock has not been set since the battery ran flat: a session
// stamped with it would look older than everything on the server.
#define CLOCK_VALID_AFTER 1577836800LL

#define BATCH_MAX 50
#define APPEND_MAX 32
#define STATE_MAX 512
#define WAKE_SECS 5
// With Wi-Fi up, waiting sessions go out this long after the last try...
#define PUSH_EVERY_SECS 20
// ...and the server is asked what changed elsewhere this often.
#define PULL_EVERY_SECS (15 * 60)
// The open session is written to the card this often while playing, so a
// power cut loses no more than that.
#define CHECKPOINT_SECS 30
// Within this of the end a book counts as finished, as on the audiobook page.
#define FINISH_MARGIN 30.0
// A move this big while paused is a seek worth sending on its own.
#define SEEK_WORTH 5.0
#define BODY_LIMIT (1024 * 1024)

typedef struct {
	char id[72];
	char item[40];
	char title[160];
	char author[112];
	double duration;
	double start;	   // where in the book it began, seconds
	double current;	   // where it got to
	double listened;   // seconds of actual playing
	long long started; // ms since 1970
	long long updated;
	int finished; // -1 nothing said, 0 not finished, 1 finished
} session_t;

typedef struct {
	char item[40];
	long long local;  // the newest session this player made for the book
	long long pulled; // the newest server position already written into the index
} item_state_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

static char card_root[512];
static char device_id[40];
static bool root_changed;

static session_t open_session; // .item[0] == '\0' when none
static bool checkpoint_due;
static session_t to_append[APPEND_MAX];
static int append_count;
static int file_count;

static item_state_t states[STATE_MAX];
static int state_count;
static bool states_dirty;

static char playing_item[40]; // the book loaded in the player, playing or paused
static bool playing_now;
static char move_item[40];	  // the loaded book's place on the server, for the GUI to go to
static double move_seconds;
static char paused_item[40];
static double paused_position;

static bool sync_requested;
static bool sending;
static long long last_sync;
static long long last_push_try;
static long long last_pull;
static char last_error[160];
static unsigned attempts;
static unsigned serial;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool clock_valid(void) { return (long long)time(NULL) >= CLOCK_VALID_AFTER; }

static double mono_secs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool online(void) {
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0];
}

static void path_of(const char *root, const char *name, char *out, size_t size) {
	snprintf(out, size, "%s/%s", root, name);
}

static void mkdirs_for(const char *file) {
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s", file);
	char *slash = strrchr(tmp, '/');
	if (!slash) {
		return;
	}
	*slash = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void json_string(FILE *f, const char *s) {
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

// One session as one line of the queue file.
static void session_write(FILE *f, const session_t *s) {
	fputs("{\"id\":", f);
	json_string(f, s->id);
	fputs(",\"item\":", f);
	json_string(f, s->item);
	fputs(",\"title\":", f);
	json_string(f, s->title);
	fputs(",\"author\":", f);
	json_string(f, s->author);
	fprintf(f, ",\"duration\":%.3f,\"start\":%.3f,\"current\":%.3f,\"listened\":%.3f,\"started\":%lld,\"updated\":%lld,"
			   "\"finished\":%d}\n",
			s->duration, s->start, s->current, s->listened, s->started, s->updated, s->finished);
}

static bool session_read(const char *line, session_t *s) {
	json_doc_t doc;
	if (!json_parse(line, &doc)) {
		return false;
	}
	int o = json_root(&doc);
	memset(s, 0, sizeof(*s));
	json_obj_str(&doc, o, "id", s->id, sizeof(s->id));
	json_obj_str(&doc, o, "item", s->item, sizeof(s->item));
	json_obj_str(&doc, o, "title", s->title, sizeof(s->title));
	json_obj_str(&doc, o, "author", s->author, sizeof(s->author));
	s->duration = json_double(&doc, json_get(&doc, o, "duration"), 0);
	s->start = json_double(&doc, json_get(&doc, o, "start"), 0);
	s->current = json_double(&doc, json_get(&doc, o, "current"), 0);
	s->listened = json_double(&doc, json_get(&doc, o, "listened"), 0);
	s->started = json_obj_llong(&doc, o, "started", 0);
	s->updated = json_obj_llong(&doc, o, "updated", 0);
	s->finished = (int)json_obj_long(&doc, o, "finished", -1);
	json_free(&doc);
	return s->id[0] && s->item[0];
}

// Every session in the queue file. The caller frees.
static session_t *queue_read(const char *root, int *count) {
	*count = 0;
	char path[600];
	path_of(root, QUEUE_FILE, path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	session_t *list = NULL;
	int capacity = 0;
	char line[2048];
	while (fgets(line, sizeof(line), f)) {
		session_t s;
		if (!session_read(line, &s)) {
			continue;
		}
		if (*count == capacity) {
			int grown = capacity ? capacity * 2 : 32;
			session_t *more = realloc(list, (size_t)grown * sizeof(*more));
			if (!more) {
				break;
			}
			list = more;
			capacity = grown;
		}
		list[(*count)++] = s;
	}
	fclose(f);
	return list;
}

static bool queue_rewrite(const char *root, const session_t *list, int count) {
	char path[600], tmp[610];
	path_of(root, QUEUE_FILE, path, sizeof(path));
	if (count == 0) {
		return unlink(path) == 0 || errno == ENOENT;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		session_write(f, &list[i]);
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// what this player and the server last said about each book, under `lock`
// ---------------------------------------------------------------------------

static item_state_t *state_for_locked(const char *item, bool create) {
	for (int i = 0; i < state_count; i++) {
		if (strcmp(states[i].item, item) == 0) {
			return &states[i];
		}
	}
	if (!create || state_count == STATE_MAX) {
		return NULL;
	}
	item_state_t *s = &states[state_count++];
	memset(s, 0, sizeof(*s));
	snprintf(s->item, sizeof(s->item), "%s", item);
	return s;
}

static void states_load(const char *root) {
	char path[600];
	path_of(root, STATE_FILE, path, sizeof(path));
	FILE *f = fopen(path, "r");
	pthread_mutex_lock(&lock);
	state_count = 0;
	if (f) {
		char item[40];
		long long local, pulled;
		while (state_count < STATE_MAX && fscanf(f, "%39s %lld %lld", item, &local, &pulled) == 3) {
			item_state_t *s = state_for_locked(item, true);
			s->local = local;
			s->pulled = pulled;
		}
		fclose(f);
	}
	pthread_mutex_unlock(&lock);
}

static void states_save(const char *root) {
	item_state_t copy[STATE_MAX];
	pthread_mutex_lock(&lock);
	int n = state_count;
	memcpy(copy, states, (size_t)n * sizeof(*copy));
	states_dirty = false;
	pthread_mutex_unlock(&lock);

	char path[600], tmp[610];
	path_of(root, STATE_FILE, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	mkdirs_for(path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	for (int i = 0; i < n; i++) {
		fprintf(f, "%s %lld %lld\n", copy[i].item, copy[i].local, copy[i].pulled);
	}
	bool ok = fclose(f) == 0;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
	}
}

// ---------------------------------------------------------------------------
// gathering sessions, on the GUI thread
// ---------------------------------------------------------------------------

static void bump_locked(void) { serial++; }

static void queue_session_locked(const session_t *s) {
	if (append_count < APPEND_MAX) {
		to_append[append_count++] = *s;
	}
	// The newest this player said about the book: what the server's position
	// has to beat before it is written over this player's own. With the clock
	// unknown, never: what was listened to may be newer than anything.
	item_state_t *st = state_for_locked(s->item, true);
	if (st) {
		st->local = s->updated > 1 ? s->updated : 0x3fffffffffffffffLL;
		states_dirty = true;
	}
	bump_locked();
	pthread_cond_signal(&wake);
}

static void start_session_locked(const char *item, double at) {
	absdl_entry_t d;
	memset(&open_session, 0, sizeof(open_session));
	snprintf(open_session.item, sizeof(open_session.item), "%s", item);
	if (absdl_get(item, &d)) {
		snprintf(open_session.title, sizeof(open_session.title), "%s", d.title);
		snprintf(open_session.author, sizeof(open_session.author), "%s", d.author);
		open_session.duration = d.duration;
	}
	long long ms = now_ms();
	snprintf(open_session.id, sizeof(open_session.id), "sonix-%s-%lld-%04x", device_id, ms, (unsigned)(rand() & 0xffff));
	open_session.start = open_session.current = at;
	open_session.started = open_session.updated = clock_valid() ? ms : 1;
	open_session.finished = -1;
	bump_locked();
}

static void end_session_locked(void) {
	session_t *s = &open_session;
	if (!s->item[0]) {
		return;
	}
	if (s->listened >= 1.0 || s->current != s->start) {
		if (s->duration > 0 && s->current >= s->duration - FINISH_MARGIN) {
			s->finished = 1;
		}
		queue_session_locked(s);
	}
	snprintf(paused_item, sizeof(paused_item), "%.39s", s->item);
	paused_position = s->current;
	s->item[0] = '\0';
	checkpoint_due = true; // the open file goes
	bump_locked();
}

void abssync_note_playback(const char *file, bool playing, double position) {
	static double last_tick;
	static double last_checkpoint;
	double now = mono_secs();
	double elapsed = last_tick > 0 ? now - last_tick : 0;
	last_tick = now;
	if (elapsed < 0 || elapsed > 3) {
		elapsed = 0; // a pause of the GUI, or the player asleep: nothing was heard
	}

	char item[40] = "";
	double at = 0;
	bool ours = file && file[0] && absdl_item_for_file(file, item, sizeof(item), position, &at);

	pthread_mutex_lock(&lock);
	if (!card_root[0]) {
		pthread_mutex_unlock(&lock);
		return;
	}
	if (open_session.item[0] && (!ours || !playing || strcmp(open_session.item, item) != 0)) {
		end_session_locked();
	}
	if (ours && playing) {
		if (!open_session.item[0]) {
			start_session_locked(item, at);
			last_checkpoint = now;
		}
		open_session.listened += elapsed;
		open_session.current = at;
		open_session.updated = clock_valid() ? now_ms() : 1;
		if (now - last_checkpoint >= CHECKPOINT_SECS) {
			last_checkpoint = now;
			checkpoint_due = true;
			pthread_cond_signal(&wake);
		}
	} else if (ours && !playing) {
		// Moved while paused -- a chapter picked, a skip: worth sending too.
		if (strcmp(paused_item, item) != 0) {
			snprintf(paused_item, sizeof(paused_item), "%s", item);
			paused_position = at;
		} else if (at - paused_position > SEEK_WORTH || paused_position - at > SEEK_WORTH) {
			start_session_locked(item, paused_position);
			open_session.current = at;
			end_session_locked();
		}
	}
	snprintf(playing_item, sizeof(playing_item), "%s", ours ? item : "");
	playing_now = ours && playing;
	pthread_mutex_unlock(&lock);
}

void abssync_mark_finished(const char *item_id, bool finished) {
	pthread_mutex_lock(&lock);
	if (card_root[0] && item_id && item_id[0]) {
		session_t keep = open_session;
		start_session_locked(item_id, 0);
		session_t *s = &open_session;
		s->finished = finished ? 1 : 0;
		s->current = finished ? s->duration : 0;
		queue_session_locked(s);
		open_session = keep;
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// sending, on the worker
// ---------------------------------------------------------------------------

static void append_waiting(const char *root) {
	session_t list[APPEND_MAX];
	pthread_mutex_lock(&lock);
	int n = append_count;
	memcpy(list, to_append, (size_t)n * sizeof(*list));
	append_count = 0;
	pthread_mutex_unlock(&lock);
	if (n == 0) {
		return;
	}
	char path[600];
	path_of(root, QUEUE_FILE, path, sizeof(path));
	mkdirs_for(path);
	FILE *f = fopen(path, "a");
	if (!f) {
		return;
	}
	for (int i = 0; i < n; i++) {
		session_write(f, &list[i]);
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	pthread_mutex_lock(&lock);
	file_count += n;
	bump_locked();
	pthread_mutex_unlock(&lock);
}

static void write_checkpoint(const char *root) {
	pthread_mutex_lock(&lock);
	session_t s = open_session;
	checkpoint_due = false;
	pthread_mutex_unlock(&lock);
	char path[600];
	path_of(root, OPEN_FILE, path, sizeof(path));
	if (!s.item[0]) {
		unlink(path);
		return;
	}
	mkdirs_for(path);
	FILE *f = fopen(path, "w");
	if (f) {
		session_write(f, &s);
		fclose(f);
	}
}

// A session left open by a power cut goes into the queue as it was last
// written.
static void recover_open(const char *root) {
	char path[600];
	path_of(root, OPEN_FILE, path, sizeof(path));
	char *text = read_file_content(path);
	if (!text) {
		return;
	}
	session_t s;
	if (session_read(text, &s)) {
		pthread_mutex_lock(&lock);
		queue_session_locked(&s);
		pthread_mutex_unlock(&lock);
		append_waiting(root);
	}
	free(text);
	unlink(path);
}

static void day_of(long long ms, char *date, size_t date_size, char *weekday, size_t weekday_size) {
	static const char *const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	time_t t = (time_t)(ms / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(date, date_size, "%Y-%m-%d", &tm);
	snprintf(weekday, weekday_size, "%s", DAYS[tm.tm_wday % 7]);
}

// POST /api/session/local-all with `count` sessions. Writes which ones the
// server took into `taken`. False when it did not answer.
static bool push_batch(const session_t *list, int count, bool *taken, char *error, size_t error_size) {
	char *body = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&body, &size);
	if (!f) {
		return false;
	}
	fputs("{\"sessions\":[", f);
	for (int i = 0; i < count; i++) {
		const session_t *s = &list[i];
		char date[16], weekday[16];
		day_of(s->started, date, sizeof(date), weekday, sizeof(weekday));
		fputs(i ? ",{\"id\":" : "{\"id\":", f);
		json_string(f, s->id);
		fputs(",\"libraryItemId\":", f);
		json_string(f, s->item);
		fputs(",\"mediaType\":\"book\",\"displayTitle\":", f);
		json_string(f, s->title);
		fputs(",\"displayAuthor\":", f);
		json_string(f, s->author);
		fprintf(f,
				",\"duration\":%.3f,\"playMethod\":3,\"mediaPlayer\":\"sonix\",\"deviceInfo\":{\"deviceId\":\"%s\","
				"\"clientName\":\"Sonix Player\",\"manufacturer\":\"HiBy\"},\"startTime\":%.3f,\"currentTime\":%.3f,"
				"\"timeListening\":%.0f,\"dayOfWeek\":\"%s\",\"date\":\"%s\",\"startedAt\":%lld,\"updatedAt\":%lld}",
				s->duration, device_id, s->start, s->current, s->listened, weekday, date, s->started, s->updated);
	}
	fputs("]}", f);
	fclose(f);

	abs_req_t req = {.method = "POST", .path = "/api/session/local-all", .json_body = body, .limit = BODY_LIMIT};
	char *answer = NULL;
	int status = 0;
	bool answered = abs_request(&req, &answer, NULL, &status);
	free(body);
	if (!answered || status != 200) {
		const char *why = http_last_error();
		snprintf(error, error_size, "%s",
				 status == 401 ? "abs_expired" : answered ? "abs_sync_refused" : why && why[0] ? why : "abs_no_reply");
		free(answer);
		return false;
	}
	json_doc_t doc;
	if (json_parse(answer, &doc)) {
		int results = json_get(&doc, json_root(&doc), "results");
		for (int i = 0; i < json_len(&doc, results); i++) {
			int r = json_at(&doc, results, i);
			char id[72];
			if (!json_obj_str(&doc, r, "id", id, sizeof(id)) || !json_obj_bool(&doc, r, "success", false)) {
				continue;
			}
			for (int k = 0; k < count; k++) {
				if (strcmp(list[k].id, id) == 0) {
					taken[k] = true;
				}
			}
		}
		json_free(&doc);
	}
	free(answer);
	return true;
}

typedef struct {
	char item[40];
	double current;
	bool finished;
	long long updated;
} server_progress_t;

// Every book the user has a place in, from GET /api/me. The caller frees.
static server_progress_t *read_server(int *count) {
	*count = 0;
	abs_req_t req = {.path = "/api/me", .limit = 4 * 1024 * 1024};
	char *body = NULL;
	int status = 0;
	if (!abs_request(&req, &body, NULL, &status) || status != 200) {
		free(body);
		return NULL;
	}
	server_progress_t *list = NULL;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int arr = json_get(&doc, json_root(&doc), "mediaProgress");
		int n = json_len(&doc, arr);
		list = n > 0 ? calloc((size_t)n, sizeof(*list)) : NULL;
		for (int i = 0; list && i < n; i++) {
			int p = json_at(&doc, arr, i);
			server_progress_t *s = &list[*count];
			json_obj_str(&doc, p, "libraryItemId", s->item, sizeof(s->item));
			s->current = json_double(&doc, json_get(&doc, p, "currentTime"), 0);
			s->finished = json_obj_bool(&doc, p, "isFinished", false);
			s->updated = json_obj_llong(&doc, p, "lastUpdate", 0);
			if (s->item[0]) {
				(*count)++;
			}
		}
		json_free(&doc);
	}
	free(body);
	return list;
}

// "Finished" and "not finished" are said with PATCH, which -- unlike the
// sessions -- writes over whatever the server has. So only when what the server
// has is older.
static void push_finished(const session_t *s, const server_progress_t *server, int server_count) {
	for (int i = 0; i < server_count; i++) {
		if (strcmp(server[i].item, s->item) == 0 && server[i].updated > s->updated) {
			return;
		}
	}
	char path[96], body[256];
	snprintf(path, sizeof(path), "/api/me/progress/%s", s->item);
	snprintf(body, sizeof(body),
			 "{\"isFinished\":%s,\"currentTime\":%.3f,\"duration\":%.3f,\"progress\":%s,\"lastUpdate\":%lld}",
			 s->finished ? "true" : "false", s->current, s->duration, s->finished ? "1" : "0", s->updated);
	abs_req_t req = {.method = "PATCH", .path = path, .json_body = body, .limit = 64 * 1024};
	char *answer = NULL;
	int status = 0;
	abs_request(&req, &answer, NULL, &status);
	free(answer);
}

// The server's place in each downloaded book, written into the audiobook index
// when it is newer than anything this player said -- someone listened
// elsewhere. Never for the book playing now: the player owns that one, and it
// is looked at again once it pauses. The book loaded and paused is the usual
// case -- the phone in the car, the player on the desk -- and the player itself
// is moved there as well.
static void pull(const char *root, const server_progress_t *server, int count) {
	(void)root;
	for (int i = 0; i < count; i++) {
		const server_progress_t *s = &server[i];
		absdl_entry_t d;
		if (!absdl_get(s->item, &d) || d.state != ABSDL_DONE) {
			continue;
		}
		pthread_mutex_lock(&lock);
		item_state_t *st = state_for_locked(s->item, true);
		bool loaded = strcmp(playing_item, s->item) == 0;
		bool newer = st && s->updated > st->local && s->updated > st->pulled && !(loaded && playing_now);
		pthread_mutex_unlock(&lock);
		if (!newer) {
			continue;
		}
		char file[900], book[900];
		double seconds = 0;
		if (absdl_file_at(s->item, s->finished ? 0 : s->current, file, sizeof(file), &seconds) &&
			audiobookdb_book_for_file(file, book, sizeof(book))) {
			if (s->finished) {
				audiobookdb_mark_finished(book);
			} else {
				audiobookdb_save_position(book, file, seconds);
			}
			printf("abssync: %s is at %.0f s on the server, newer than here\n", d.title, s->current);
			if (loaded && !s->finished) {
				pthread_mutex_lock(&lock);
				snprintf(move_item, sizeof(move_item), "%s", s->item);
				move_seconds = s->current;
				pthread_mutex_unlock(&lock);
			}
		}
		pthread_mutex_lock(&lock);
		st = state_for_locked(s->item, true);
		if (st) {
			st->pulled = s->updated;
			states_dirty = true;
		}
		pthread_mutex_unlock(&lock);
	}
}

static void do_sync(const char *root, bool pull_too) {
	pthread_mutex_lock(&lock);
	sending = true;
	bump_locked();
	pthread_mutex_unlock(&lock);

	char error[160] = "";
	bool ok = true;
	int count = 0;
	session_t *list = queue_read(root, &count);
	int server_count = 0;
	server_progress_t *server = NULL;

	if (count > 0) {
		bool *taken = calloc((size_t)count, sizeof(bool));
		for (int from = 0; taken && from < count && ok; from += BATCH_MAX) {
			int n = count - from < BATCH_MAX ? count - from : BATCH_MAX;
			ok = push_batch(list + from, n, taken + from, error, sizeof(error));
		}
		// What the server took leaves the queue; the rest waits for next time.
		int kept = 0;
		bool any_finish = false;
		for (int i = 0; taken && i < count; i++) {
			if (taken[i]) {
				any_finish |= list[i].finished >= 0;
			}
		}
		if (any_finish) {
			server = read_server(&server_count);
		}
		for (int i = 0; taken && i < count; i++) {
			if (!taken[i]) {
				list[kept++] = list[i];
			} else if (list[i].finished >= 0) {
				push_finished(&list[i], server, server_count);
			}
		}
		if (taken) {
			queue_rewrite(root, list, kept);
			pthread_mutex_lock(&lock);
			file_count = kept;
			pthread_mutex_unlock(&lock);
		}
		free(taken);
	}
	free(list);

	if (ok && pull_too) {
		free(server);
		server = read_server(&server_count);
		if (server || server_count == 0) {
			pull(root, server, server_count);
		}
	}
	free(server);

	pthread_mutex_lock(&lock);
	sending = false;
	if (ok) {
		last_sync = (long long)time(NULL);
		last_error[0] = '\0';
	} else {
		snprintf(last_error, sizeof(last_error), "%s", error);
	}
	attempts++;
	bool dirty = states_dirty;
	bump_locked();
	pthread_mutex_unlock(&lock);
	if (dirty) {
		states_save(root);
	}
}

static void *worker(void *arg) {
	(void)arg;
	thread_be_background("abs sync");
	for (;;) {
		pthread_mutex_lock(&lock);
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += WAKE_SECS;
		while (!root_changed && !sync_requested && append_count == 0 && !checkpoint_due) {
			if (pthread_cond_timedwait(&wake, &lock, &until) == ETIMEDOUT) {
				break;
			}
		}
		char root[512];
		snprintf(root, sizeof(root), "%s", card_root);
		bool fresh_root = root_changed;
		root_changed = false;
		bool asked = sync_requested;
		sync_requested = false;
		bool checkpoint = checkpoint_due;
		int waiting = file_count + append_count;
		bool dirty = states_dirty;
		pthread_mutex_unlock(&lock);

		if (!root[0]) {
			continue;
		}
		if (fresh_root) {
			states_load(root);
			int n = 0;
			free(queue_read(root, &n));
			pthread_mutex_lock(&lock);
			file_count = n;
			bump_locked();
			pthread_mutex_unlock(&lock);
			recover_open(root);
		}
		append_waiting(root);
		if (checkpoint) {
			write_checkpoint(root);
		}
		if (dirty) {
			states_save(root);
		}

		abs_status_t account;
		abs_get_status(&account);
		if (account.state != ABS_STATE_SIGNED_IN || !online()) {
			if (asked) {
				pthread_mutex_lock(&lock);
				snprintf(last_error, sizeof(last_error), "%s",
						 account.state == ABS_STATE_EXPIRED ? "abs_expired" : "enable_wi_fi_first");
				attempts++;
				bump_locked();
				pthread_mutex_unlock(&lock);
			}
			continue;
		}
		long long now = (long long)time(NULL);
		bool push_due = waiting > 0 && now - last_push_try >= PUSH_EVERY_SECS;
		bool pull_due = now - last_pull >= PULL_EVERY_SECS;
		if (asked || push_due || pull_due) {
			last_push_try = now;
			if (asked || pull_due) {
				last_pull = now;
			}
			do_sync(root, asked || pull_due);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void abssync_set_root(const char *sd_root) {
	// An id for this player, made once: the server lists sessions by device.
	const char *id = config_get("audiobookshelf", "device_id", "");
	char made[40] = "";
	if (!id[0]) {
		srand((unsigned)(time(NULL) ^ (long)getpid()));
		snprintf(made, sizeof(made), "%08x%08x", (unsigned)rand(), (unsigned)rand());
		config_set("audiobookshelf", "device_id", made);
		config_save();
		id = made;
	}
	pthread_mutex_lock(&lock);
	snprintf(device_id, sizeof(device_id), "%s", id);
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	root_changed = true;
	if (!started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker, NULL) == 0) {
			pthread_detach(thread);
			started = true;
		}
	}
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void abssync_now(void) {
	pthread_mutex_lock(&lock);
	// What is playing goes too: the open session is closed and a new one starts
	// at the next tick.
	end_session_locked();
	last_error[0] = '\0';
	sync_requested = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void abssync_get_status(abssync_status_t *out) {
	pthread_mutex_lock(&lock);
	memset(out, 0, sizeof(*out));
	out->pending = file_count + append_count + (open_session.item[0] ? 1 : 0);
	out->sending = sending;
	out->last_sync = last_sync;
	snprintf(out->error, sizeof(out->error), "%s", last_error);
	out->attempts = attempts;
	pthread_mutex_unlock(&lock);
}

bool abssync_take_move(char *item_out, size_t item_size, double *book_seconds) {
	pthread_mutex_lock(&lock);
	bool have = move_item[0] != '\0';
	if (have) {
		snprintf(item_out, item_size, "%s", move_item);
		*book_seconds = move_seconds;
		move_item[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
	return have;
}

unsigned abssync_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned s = serial;
	pthread_mutex_unlock(&lock);
	return s;
}

bool abssync_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool busy = sending;
	pthread_mutex_unlock(&lock);
	return busy;
}
