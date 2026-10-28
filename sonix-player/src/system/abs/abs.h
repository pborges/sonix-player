#ifndef ABS_H
#define ABS_H

#include <stdbool.h>
#include <stddef.h>

// The Audiobookshelf account: which server, who is signed in, and the tokens
// every request to it carries.
//
// Audiobookshelf is a server people run for themselves. This player does not
// stream from it: books are downloaded into the Audiobooks folder and played
// by the audiobook player like any other, and what goes back to the server is
// how far each one got. This file is only the account and the signed request
// the rest is built on.
//
// Two ways in:
//
//   password  POST /login with "x-return-tokens: true" trades the user name
//             and password for an access token (an hour) and a refresh token
//             (thirty days). Each refresh hands out a new pair, so a player
//             that connects at least once a month never has to sign in again;
//             one left in a drawer for longer does.
//   API key   made by an admin in the server's settings, and it does not
//             expire. The better fit for a player that is offline for weeks.
//
// The password is never kept. The server address, the user and the tokens are
// kept beside device_config.ini, readable by this process only.
//
// Servers older than 2.26 hand out a token that never expires and do not know
// refresh tokens. They are refused at sign-in rather than half supported.
//
// A worker thread does the signing in; abs_request() runs on whatever thread
// calls it, which is never the GUI thread.

#define ABS_MIN_VERSION "2.26.0"

typedef enum {
	ABS_STATE_SIGNED_OUT,
	ABS_STATE_SIGNING_IN,
	ABS_STATE_SIGNED_IN,
	// The server refused the tokens and they could not be renewed: the refresh
	// token ran out, or the API key was deleted. The account is still the
	// account -- what is waiting to be sent stays -- but it needs signing in.
	ABS_STATE_EXPIRED,
} abs_state_t;

typedef enum {
	ABS_LOGIN_OK,
	ABS_LOGIN_WRONG_CREDENTIALS,
	ABS_LOGIN_NO_REPLY,	  // nothing answered; `detail` may say why (a certificate, the clock)
	ABS_LOGIN_NOT_ABS,	  // something answered, and it is not Audiobookshelf
	ABS_LOGIN_TOO_OLD,	  // `detail` holds the server's version
	ABS_LOGIN_NO_PASSWORD, // the server only allows signing in through a web page (OpenID)
	ABS_LOGIN_REFUSED,	  // anything else; `detail` says what
} abs_login_result_t;

typedef enum {
	ABS_AUTH_PASSWORD,
	ABS_AUTH_API_KEY,
} abs_auth_t;

typedef struct {
	abs_state_t state;
	abs_auth_t auth;
	char server[256]; // as normalised: scheme, host, port, no trailing slash
	char user[128];
	char version[16]; // the server's, as of the last sign-in
	char detail[160];

	// Moves each time a sign-in attempt finishes; `login_result` is how it did.
	unsigned login_serial;
	abs_login_result_t login_result;
} abs_status_t;

// Reads the saved account and starts the worker. Once, after config_init().
void abs_init(void);

// An account is set up: signed in, or signed in once and waiting to be
// renewed. What decides whether the Audiobooks page shows its Sync tile.
bool abs_configured(void);

// All three return at once; the worker does the work. Secrets are wiped as
// soon as the request has been built.
void abs_login_password(const char *server, const char *user, const char *password);
void abs_login_api_key(const char *server, const char *api_key);
void abs_logout(void);

void abs_get_status(abs_status_t *out);

// Bumped whenever abs_configured() may have changed, so a page can rebuild
// only when it has to.
unsigned abs_account_serial(void);

// Turns what was typed into the address requests go to: spaces trimmed,
// "https://" added when no scheme was given, trailing slashes removed. False
// when nothing usable is left.
bool abs_normalise_server(const char *in, char *out, size_t size);

// ---------------------------------------------------------------------------
// For the modules built on top. Never from the GUI thread.
// ---------------------------------------------------------------------------

typedef struct {
	const char *method;		  // NULL for GET
	const char *path;		  // "/api/me", "/api/libraries?..."
	const char *json_body;	  // NULL for none
	size_t limit;			  // largest body kept, 0 for the default
	int timeout_secs;		  // 0 for the default
} abs_req_t;

// A signed request to the server. When the access token has run out it is
// renewed and the request sent once more. True with any answer from the
// server, *status telling which; *out is then allocated and the caller frees
// it. False when there was no answer, or no account.
bool abs_request(const abs_req_t *req, char **out, size_t *out_len, int *status);

// "Authorization: Bearer ...\r\n" for http_stream_open_ex(), and the server
// address to put in front of a path. False when signed out.
bool abs_auth_header(char *out, size_t size);
bool abs_server(char *out, size_t size);

// A download got 401: renew the token now. True when there is a fresh one to
// try with.
bool abs_renew(void);

// True while a request is on the wire, so the Wi-Fi is not parked under it.
bool abs_network_wanted(void);

#endif /* ABS_H */
