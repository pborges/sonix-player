#define _GNU_SOURCE 1

#include "abs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/net/http.h"

#define TIMEOUT_SECS 15
#define BODY_LIMIT (4 * 1024 * 1024)
#define LOGIN_BODY_LIMIT (256 * 1024)

// A JWT from this server is about three hundred characters. Room for a server
// that one day puts more in them.
#define TOKEN_MAX 2048

// ---------------------------------------------------------------------------
// state shared with the worker, under `lock`
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool started;

static abs_state_t state;
static abs_auth_t auth;
static char server[256];
static char user[128];
static char version[16];
static char detail[160];
static char access_token[TOKEN_MAX];
static char refresh_token[TOKEN_MAX];
static unsigned account_serial;

static bool login_requested;
static bool logout_requested;
static abs_auth_t login_auth;
static char login_server[256];
static char login_user[128];
static char login_secret[TOKEN_MAX]; // the password, or the API key
static unsigned login_serial;
static abs_login_result_t login_result;

static int requests_in_flight;

static char session_path[512];

// One renewal at a time: two downloads that get 401 together must not both
// spend the same refresh token.
static pthread_mutex_t renew_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void wipe(void *p, size_t n) {
	volatile unsigned char *b = p;
	while (n--) {
		*b++ = 0;
	}
}

// `in` as the inside of a JSON string.
static void json_escape(const char *in, char *out, size_t size) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < size; p++) {
		if (*p == '"' || *p == '\\') {
			out[o++] = '\\';
			out[o++] = (char)*p;
		} else if (*p < 0x20) {
			o += (size_t)snprintf(out + o, size - o, "\\u%04x", *p);
		} else {
			out[o++] = (char)*p;
		}
	}
	out[o] = '\0';
}

// "2.37.1" against "2.26.0". Parts that are missing count as zero; a suffix
// such as "-beta" is ignored.
static int version_compare(const char *a, const char *b) {
	int va[3] = {0}, vb[3] = {0};
	sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
	sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
	for (int i = 0; i < 3; i++) {
		if (va[i] != vb[i]) {
			return va[i] < vb[i] ? -1 : 1;
		}
	}
	return 0;
}

bool abs_normalise_server(const char *in, char *out, size_t size) {
	if (!in || size < 16) {
		return false;
	}
	while (isspace((unsigned char)*in)) {
		in++;
	}
	size_t len = strlen(in);
	while (len > 0 && (isspace((unsigned char)in[len - 1]) || in[len - 1] == '/')) {
		len--;
	}
	if (len == 0) {
		return false;
	}
	bool has_scheme = (len >= 7 && strncasecmp(in, "http://", 7) == 0) || (len >= 8 && strncasecmp(in, "https://", 8) == 0);
	int n = snprintf(out, size, "%s%.*s", has_scheme ? "" : "https://", (int)len, in);
	if (n <= 0 || (size_t)n >= size) {
		return false;
	}
	// The scheme in lower case: the rest of the player compares it that way.
	for (char *p = out; *p && *p != ':'; p++) {
		*p = (char)tolower((unsigned char)*p);
	}
	// Something has to follow the scheme.
	return strstr(out, "://")[3] != '\0';
}

// ---------------------------------------------------------------------------
// the session file, beside device_config.ini: /usr/data on the device
// ---------------------------------------------------------------------------

static void build_paths(void) {
	char dir[400] = "/tmp";
	const char *cfg = config_path();
	if (cfg && cfg[0]) {
		const char *slash = strrchr(cfg, '/');
		if (slash == cfg) {
			snprintf(dir, sizeof(dir), "/");
		} else if (slash) {
			snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cfg), cfg);
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}
	snprintf(session_path, sizeof(session_path), "%s/abs.session", dir);
}

// Under `lock`.
static void load_session_locked(void) {
	state = ABS_STATE_SIGNED_OUT;
	auth = ABS_AUTH_PASSWORD;
	server[0] = user[0] = version[0] = '\0';
	wipe(access_token, sizeof(access_token));
	wipe(refresh_token, sizeof(refresh_token));

	FILE *f = fopen(session_path, "r");
	if (!f) {
		return;
	}
	char line[TOKEN_MAX + 32];
	bool expired = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *k = line, *v = eq + 1;
		if (strcmp(k, "server") == 0) {
			snprintf(server, sizeof(server), "%s", v);
		} else if (strcmp(k, "user") == 0) {
			snprintf(user, sizeof(user), "%s", v);
		} else if (strcmp(k, "version") == 0) {
			snprintf(version, sizeof(version), "%s", v);
		} else if (strcmp(k, "auth") == 0) {
			auth = strcmp(v, "api_key") == 0 ? ABS_AUTH_API_KEY : ABS_AUTH_PASSWORD;
		} else if (strcmp(k, "access") == 0) {
			snprintf(access_token, sizeof(access_token), "%s", v);
		} else if (strcmp(k, "refresh") == 0) {
			snprintf(refresh_token, sizeof(refresh_token), "%s", v);
		} else if (strcmp(k, "expired") == 0) {
			expired = strcmp(v, "1") == 0;
		}
	}
	wipe(line, sizeof(line));
	fclose(f);

	if (!server[0] || !user[0]) {
		state = ABS_STATE_SIGNED_OUT;
	} else if (expired || !access_token[0]) {
		state = ABS_STATE_EXPIRED;
	} else {
		state = ABS_STATE_SIGNED_IN;
	}
}

// Under `lock`. Written to a temporary file and renamed, so a power cut leaves
// the old session or the new one, never half of each.
static void save_session_locked(void) {
	if (state == ABS_STATE_SIGNED_OUT) {
		unlink(session_path);
		return;
	}
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", session_path);
	// Tokens are as good as the password: nobody else reads this file.
	mode_t old_mask = umask(077);
	FILE *f = fopen(tmp, "w");
	umask(old_mask);
	if (!f) {
		fprintf(stderr, "abs: cannot write %s\n", tmp);
		return;
	}
	fprintf(f, "server=%s\nuser=%s\nversion=%s\nauth=%s\naccess=%s\nrefresh=%s\nexpired=%d\n", server, user, version,
			auth == ABS_AUTH_API_KEY ? "api_key" : "password", access_token, refresh_token,
			state == ABS_STATE_EXPIRED ? 1 : 0);
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, session_path) != 0) {
		fprintf(stderr, "abs: cannot write %s\n", session_path);
		unlink(tmp);
	}
}

// ---------------------------------------------------------------------------
// talking to the server
// ---------------------------------------------------------------------------

static void network_begin(void) {
	pthread_mutex_lock(&lock);
	requests_in_flight++;
	pthread_mutex_unlock(&lock);
}

static void network_end(void) {
	pthread_mutex_lock(&lock);
	requests_in_flight--;
	pthread_mutex_unlock(&lock);
}

// One request, unsigned unless `bearer` is given. True with any answer.
static bool send_request(const char *base, const char *method, const char *path, const char *extra_headers,
						 const char *bearer, const char *json_body, size_t limit, int timeout_secs, char **out,
						 size_t *out_len, int *status) {
	char url[2600];
	snprintf(url, sizeof(url), "%s%s", base, path);

	size_t headers_size = (extra_headers ? strlen(extra_headers) : 0) + (bearer ? strlen(bearer) : 0) + 64;
	char *headers = malloc(headers_size);
	if (!headers) {
		return false;
	}
	snprintf(headers, headers_size, "%s%s%s%s", extra_headers ? extra_headers : "", bearer ? "Authorization: Bearer " : "",
			 bearer ? bearer : "", bearer ? "\r\n" : "");

	http_req_t req = {
		.method = method,
		.extra_headers = headers,
		.body = json_body,
		.content_type = json_body ? "application/json" : NULL,
		.want_error_body = true,
		.allow_empty_body = true,
	};
	*status = 0;
	network_begin();
	bool ok = http_request(url, &req, out, out_len, limit ? limit : BODY_LIMIT, timeout_secs ? timeout_secs : TIMEOUT_SECS,
						   status);
	network_end();
	wipe(headers, headers_size);
	free(headers);
	return ok;
}

// The tokens out of a /login or /auth/refresh answer. False when either is
// missing.
static bool read_tokens(const char *body, char *access_out, char *refresh_out, char *user_out, size_t user_size) {
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		return false;
	}
	int u = json_get(&doc, json_root(&doc), "user");
	bool ok = json_obj_str(&doc, u, "accessToken", access_out, TOKEN_MAX) && access_out[0] &&
			  json_obj_str(&doc, u, "refreshToken", refresh_out, TOKEN_MAX) && refresh_out[0];
	if (ok && user_out) {
		json_obj_str(&doc, u, "username", user_out, user_size);
	}
	json_free(&doc);
	return ok;
}

// What GET /status says about the server: that it is Audiobookshelf, which
// version, and whether it takes a password at all.
static abs_login_result_t probe_server(const char *base, char *version_out, size_t version_size, char *why,
									   size_t why_size) {
	char *body = NULL;
	int status = 0;
	if (!send_request(base, NULL, "/status", NULL, NULL, NULL, LOGIN_BODY_LIMIT, 0, &body, NULL, &status)) {
		const char *err = http_last_error();
		snprintf(why, why_size, "%s", err ? err : "");
		return ABS_LOGIN_NO_REPLY;
	}
	abs_login_result_t result = ABS_LOGIN_NOT_ABS;
	json_doc_t doc;
	if (status == 200 && json_parse(body, &doc)) {
		int root = json_root(&doc);
		char app[32] = "";
		json_obj_str(&doc, root, "app", app, sizeof(app));
		json_obj_str(&doc, root, "serverVersion", version_out, version_size);
		if (strcmp(app, "audiobookshelf") == 0 && version_out[0]) {
			result = ABS_LOGIN_OK;
			if (version_compare(version_out, ABS_MIN_VERSION) < 0) {
				snprintf(why, why_size, "%s", version_out);
				result = ABS_LOGIN_TOO_OLD;
			} else {
				int methods = json_get(&doc, root, "authMethods");
				bool local = methods < 0; // a server that does not say takes a password
				for (int i = 0; i < json_len(&doc, methods); i++) {
					char m[32];
					if (json_str(&doc, json_at(&doc, methods, i), m, sizeof(m)) && strcmp(m, "local") == 0) {
						local = true;
					}
				}
				if (!local) {
					result = ABS_LOGIN_NO_PASSWORD;
				}
			}
		}
		json_free(&doc);
	}
	free(body);
	return result;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static void finish_login(abs_login_result_t result, const char *why) {
	pthread_mutex_lock(&lock);
	login_result = result;
	if (why) {
		snprintf(detail, sizeof(detail), "%s", why);
	}
	if (result != ABS_LOGIN_OK) {
		// A failed attempt leaves the account that was there before as it was.
		load_session_locked();
	}
	login_serial++;
	account_serial++;
	pthread_mutex_unlock(&lock);
}

static void do_login(void) {
	char base[256], name[128], secret[TOKEN_MAX];
	abs_auth_t how;
	pthread_mutex_lock(&lock);
	snprintf(base, sizeof(base), "%s", login_server);
	snprintf(name, sizeof(name), "%s", login_user);
	snprintf(secret, sizeof(secret), "%s", login_secret);
	wipe(login_secret, sizeof(login_secret));
	how = login_auth;
	pthread_mutex_unlock(&lock);

	char found_version[16] = "", why[160] = "";
	abs_login_result_t probe = probe_server(base, found_version, sizeof(found_version), why, sizeof(why));
	// An API key works whatever the server allows passwords for.
	if (probe == ABS_LOGIN_NO_PASSWORD && how == ABS_AUTH_API_KEY) {
		probe = ABS_LOGIN_OK;
	}
	if (probe != ABS_LOGIN_OK) {
		wipe(secret, sizeof(secret));
		finish_login(probe, why);
		return;
	}

	static char got_access[TOKEN_MAX], got_refresh[TOKEN_MAX];
	got_access[0] = got_refresh[0] = '\0';
	char got_user[128] = "";
	char *body = NULL;
	int status = 0;
	bool answered;

	if (how == ABS_AUTH_PASSWORD) {
		char user_json[300], password_json[TOKEN_MAX + 64], request[TOKEN_MAX + 512];
		json_escape(name, user_json, sizeof(user_json));
		json_escape(secret, password_json, sizeof(password_json));
		snprintf(request, sizeof(request), "{\"username\":\"%s\",\"password\":\"%s\"}", user_json, password_json);
		answered = send_request(base, "POST", "/login", "x-return-tokens: true\r\n", NULL, request, LOGIN_BODY_LIMIT, 0,
								&body, NULL, &status);
		wipe(password_json, sizeof(password_json));
		wipe(request, sizeof(request));
		if (answered && status == 200 && !read_tokens(body, got_access, got_refresh, got_user, sizeof(got_user))) {
			status = -1;
		}
	} else {
		answered = send_request(base, NULL, "/api/me", NULL, secret, NULL, LOGIN_BODY_LIMIT, 0, &body, NULL, &status);
		if (answered && status == 200) {
			json_doc_t doc;
			if (json_parse(body, &doc)) {
				json_obj_str(&doc, json_root(&doc), "username", got_user, sizeof(got_user));
				json_free(&doc);
			}
			snprintf(got_access, sizeof(got_access), "%s", secret);
		}
	}
	wipe(secret, sizeof(secret));
	if (body) {
		wipe(body, strlen(body));
		free(body);
	}

	if (!answered) {
		const char *err = http_last_error();
		finish_login(ABS_LOGIN_NO_REPLY, err ? err : "");
		return;
	}
	if (status == 401 || status == 403) {
		finish_login(ABS_LOGIN_WRONG_CREDENTIALS, "");
		return;
	}
	if (status != 200 || !got_access[0]) {
		char text[64];
		snprintf(text, sizeof(text), status > 0 ? "HTTP %d" : "Unexpected answer", status);
		finish_login(ABS_LOGIN_REFUSED, text);
		return;
	}

	pthread_mutex_lock(&lock);
	state = ABS_STATE_SIGNED_IN;
	auth = how;
	snprintf(server, sizeof(server), "%s", base);
	snprintf(user, sizeof(user), "%s", got_user[0] ? got_user : name);
	snprintf(version, sizeof(version), "%s", found_version);
	snprintf(access_token, sizeof(access_token), "%s", got_access);
	snprintf(refresh_token, sizeof(refresh_token), "%s", got_refresh);
	detail[0] = '\0';
	save_session_locked();
	pthread_mutex_unlock(&lock);
	wipe(got_access, sizeof(got_access));
	wipe(got_refresh, sizeof(got_refresh));
	finish_login(ABS_LOGIN_OK, NULL);
}

static void do_logout(void) {
	char base[256], refresh[TOKEN_MAX];
	bool tell_server;
	pthread_mutex_lock(&lock);
	snprintf(base, sizeof(base), "%s", server);
	snprintf(refresh, sizeof(refresh), "%s", refresh_token);
	tell_server = auth == ABS_AUTH_PASSWORD && refresh[0];
	state = ABS_STATE_SIGNED_OUT;
	server[0] = user[0] = version[0] = detail[0] = '\0';
	wipe(access_token, sizeof(access_token));
	wipe(refresh_token, sizeof(refresh_token));
	save_session_locked();
	account_serial++;
	pthread_mutex_unlock(&lock);

	// Ends the session on the server too, when it can be reached. Not waited
	// for: without it the refresh token simply runs out in thirty days.
	if (tell_server) {
		char headers[TOKEN_MAX + 32];
		snprintf(headers, sizeof(headers), "x-refresh-token: %s\r\n", refresh);
		char *body = NULL;
		int status = 0;
		if (send_request(base, "POST", "/logout", headers, NULL, NULL, LOGIN_BODY_LIMIT, 5, &body, NULL, &status)) {
			free(body);
		}
		wipe(headers, sizeof(headers));
	}
	wipe(refresh, sizeof(refresh));
}

static void *worker(void *arg) {
	(void)arg;
	pthread_mutex_lock(&lock);
	for (;;) {
		while (!login_requested && !logout_requested) {
			pthread_cond_wait(&wake, &lock);
		}
		if (logout_requested) {
			logout_requested = false;
			pthread_mutex_unlock(&lock);
			do_logout();
			pthread_mutex_lock(&lock);
		}
		if (login_requested) {
			login_requested = false;
			pthread_mutex_unlock(&lock);
			do_login();
			pthread_mutex_lock(&lock);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void abs_init(void) {
	if (started) {
		return;
	}
	build_paths();
	pthread_mutex_lock(&lock);
	load_session_locked();
	pthread_mutex_unlock(&lock);

	pthread_cond_init(&wake, NULL);
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, NULL) == 0) {
		pthread_detach(thread);
		started = true;
	}
}

bool abs_configured(void) {
	pthread_mutex_lock(&lock);
	bool yes = state == ABS_STATE_SIGNED_IN || state == ABS_STATE_EXPIRED ||
			   (state == ABS_STATE_SIGNING_IN && server[0] && user[0]);
	pthread_mutex_unlock(&lock);
	return yes;
}

static void request_login(abs_auth_t how, const char *srv, const char *name, const char *secret) {
	pthread_mutex_lock(&lock);
	if (!abs_normalise_server(srv, login_server, sizeof(login_server))) {
		login_server[0] = '\0';
	}
	login_auth = how;
	snprintf(login_user, sizeof(login_user), "%s", name ? name : "");
	snprintf(login_secret, sizeof(login_secret), "%s", secret ? secret : "");
	state = ABS_STATE_SIGNING_IN;
	login_requested = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void abs_login_password(const char *srv, const char *name, const char *password) {
	request_login(ABS_AUTH_PASSWORD, srv, name, password);
}

void abs_login_api_key(const char *srv, const char *api_key) { request_login(ABS_AUTH_API_KEY, srv, NULL, api_key); }

void abs_logout(void) {
	pthread_mutex_lock(&lock);
	logout_requested = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void abs_get_status(abs_status_t *out) {
	pthread_mutex_lock(&lock);
	memset(out, 0, sizeof(*out));
	out->state = state;
	out->auth = auth;
	snprintf(out->server, sizeof(out->server), "%s", server);
	snprintf(out->user, sizeof(out->user), "%s", user);
	snprintf(out->version, sizeof(out->version), "%s", version);
	snprintf(out->detail, sizeof(out->detail), "%s", detail);
	out->login_serial = login_serial;
	out->login_result = login_result;
	pthread_mutex_unlock(&lock);
}

unsigned abs_account_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned s = account_serial;
	pthread_mutex_unlock(&lock);
	return s;
}

bool abs_server(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	bool ok = server[0] && state != ABS_STATE_SIGNED_OUT;
	snprintf(out, size, "%s", ok ? server : "");
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_auth_header(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	bool ok = state == ABS_STATE_SIGNED_IN && access_token[0];
	int n = ok ? snprintf(out, size, "Authorization: Bearer %s\r\n", access_token) : 0;
	pthread_mutex_unlock(&lock);
	if (!ok || n <= 0 || (size_t)n >= size) {
		if (size) {
			out[0] = '\0';
		}
		return false;
	}
	return true;
}

// Under `lock`.
static void mark_expired_locked(void) {
	if (state == ABS_STATE_SIGNED_IN) {
		state = ABS_STATE_EXPIRED;
		wipe(access_token, sizeof(access_token));
		save_session_locked();
		account_serial++;
	}
}

bool abs_renew(void) {
	pthread_mutex_lock(&renew_lock);

	char base[256], refresh[TOKEN_MAX], before[TOKEN_MAX];
	pthread_mutex_lock(&lock);
	bool can = state == ABS_STATE_SIGNED_IN && auth == ABS_AUTH_PASSWORD && refresh_token[0];
	if (state == ABS_STATE_SIGNED_IN && auth == ABS_AUTH_API_KEY) {
		// A key that is refused was deleted or switched off. Nothing to renew.
		mark_expired_locked();
	}
	snprintf(base, sizeof(base), "%s", server);
	snprintf(refresh, sizeof(refresh), "%s", refresh_token);
	snprintf(before, sizeof(before), "%s", access_token);
	pthread_mutex_unlock(&lock);
	if (!can) {
		wipe(refresh, sizeof(refresh));
		wipe(before, sizeof(before));
		pthread_mutex_unlock(&renew_lock);
		return false;
	}

	char headers[TOKEN_MAX + 32];
	snprintf(headers, sizeof(headers), "x-refresh-token: %s\r\n", refresh);
	wipe(refresh, sizeof(refresh));
	char *body = NULL;
	int status = 0;
	bool answered =
		send_request(base, "POST", "/auth/refresh", headers, NULL, NULL, LOGIN_BODY_LIMIT, 0, &body, NULL, &status);
	wipe(headers, sizeof(headers));

	static char got_access[TOKEN_MAX], got_refresh[TOKEN_MAX];
	bool renewed = answered && status == 200 && read_tokens(body, got_access, got_refresh, NULL, 0);
	if (body) {
		wipe(body, strlen(body));
		free(body);
	}

	pthread_mutex_lock(&lock);
	if (renewed && state == ABS_STATE_SIGNED_IN && strcmp(server, base) == 0) {
		snprintf(access_token, sizeof(access_token), "%s", got_access);
		snprintf(refresh_token, sizeof(refresh_token), "%s", got_refresh);
		save_session_locked();
	} else if (answered && (status == 401 || status == 403) && strcmp(access_token, before) == 0) {
		// Refused, and nobody renewed in the meantime: the refresh token ran
		// out. No answer at all is not this -- the server is just away.
		mark_expired_locked();
		renewed = false;
	} else {
		renewed = false;
	}
	pthread_mutex_unlock(&lock);
	wipe(got_access, sizeof(got_access));
	wipe(got_refresh, sizeof(got_refresh));
	wipe(before, sizeof(before));

	pthread_mutex_unlock(&renew_lock);
	return renewed;
}

bool abs_request(const abs_req_t *req, char **out, size_t *out_len, int *status) {
	for (int attempt = 0; attempt < 2; attempt++) {
		char base[256], bearer[TOKEN_MAX];
		pthread_mutex_lock(&lock);
		bool ok = state == ABS_STATE_SIGNED_IN && access_token[0];
		snprintf(base, sizeof(base), "%s", server);
		snprintf(bearer, sizeof(bearer), "%s", access_token);
		pthread_mutex_unlock(&lock);
		if (!ok) {
			return false;
		}

		bool answered = send_request(base, req->method, req->path, NULL, bearer, req->json_body, req->limit,
									 req->timeout_secs, out, out_len, status);
		wipe(bearer, sizeof(bearer));
		// The access token ran out: renew it and send the request once more.
		// When it cannot be renewed the 401 is the answer.
		if (!answered || *status != 401 || attempt > 0 || !abs_renew()) {
			return answered;
		}
		free(*out);
		*out = NULL;
	}
	return false;
}

bool abs_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool busy = requests_in_flight > 0 || state == ABS_STATE_SIGNING_IN;
	pthread_mutex_unlock(&lock);
	return busy;
}
