#ifndef ABSCATALOG_H
#define ABSCATALOG_H

#include <stdbool.h>
#include <stddef.h>

// What the Audiobookshelf server has: its libraries, their books, series and
// authors, one book in detail, and what a search finds.
//
// One request at a time on a thread of this module's own; asking for something
// new while an answer is still on its way abandons that answer, which is what
// leaving a page before it filled wants. The GUI polls abscatalog_take().
//
// Every answer is also kept on the card, in .local/abs/cache, under the MD5 of
// the request. When the server cannot be reached the kept answer is handed back
// instead, marked `offline`: the player is mostly away from Wi-Fi, and the
// list of what is on the server is worth seeing there too -- with what is on
// the card marked once downloads exist.
//
// How far the user got in each book comes from GET /api/me, read along with
// every list and kept the same way, so the rows can say "37%" or "Finished".

#define ABS_ID_MAX 40
#define ABS_TITLE_MAX 160
#define ABS_NAME_MAX 112

typedef struct {
	char id[ABS_ID_MAX];
	char title[ABS_TITLE_MAX];
	char author[ABS_NAME_MAX];
	char series[ABS_TITLE_MAX]; // "Name #3" as the server writes it, "" for none
	double sequence;			// the number after '#', 0 when there is none
	double duration;			// seconds
	long long size;				// bytes, every file of the book
	long long added;			// ms since 1970
	double progress;			// 0..1
	bool finished;
	bool started;
	bool has_cover;
} abs_book_t;

typedef struct {
	char id[ABS_ID_MAX];
	char name[ABS_TITLE_MAX];
	int books;
} abs_name_t;

typedef struct {
	char id[ABS_ID_MAX];
	char name[ABS_NAME_MAX];
} abs_library_t;

#define ABS_MAX_LIBRARIES 16

// One book in full, for its own page.
typedef struct {
	abs_book_t book;
	char narrator[ABS_NAME_MAX];
	char published[16];
	char *description; // malloc'd, plain text; NULL when none
	int chapters;
	int files;
	double current_time; // seconds, where the user is
} abs_detail_t;

typedef enum {
	ABS_LIST_BOOKS,	   // a library, optionally narrowed to an author or a series
	ABS_LIST_CONTINUE, // started and not finished, most recent first
	ABS_LIST_SEARCH,   // the books a search finds, and the books of the series it finds
} abs_list_t;

typedef enum {
	ABS_SORT_TITLE,
	ABS_SORT_AUTHOR,
	ABS_SORT_ADDED, // newest first
	ABS_SORT_SERIES, // by the number in the series, then by title
} abs_sort_t;

typedef enum {
	ABS_FILTER_NONE,
	ABS_FILTER_AUTHOR,
	ABS_FILTER_SERIES,
} abs_filter_t;

typedef enum {
	ABS_NAMES_SERIES,
	ABS_NAMES_AUTHORS,
} abs_names_t;

typedef enum {
	ABS_RESULT_NONE,	 // nothing new
	ABS_RESULT_LOADING,	 // still fetching; `loaded` and `total` say how far
	ABS_RESULT_READY,
	ABS_RESULT_FAILED,	 // no answer and nothing kept; `error` says why
} abs_result_state_t;

// Where the card is: the offline copies live there. Once at startup and on
// every card change.
void abscatalog_set_root(const char *sd_root);

// The library the lists are read from: the one picked last, else the first
// book library the server lists. Empty until the libraries have been read once.
void abscatalog_library(char *id_out, size_t id_size, char *name_out, size_t name_size);
void abscatalog_set_library(const char *id, const char *name);

// The requests. Each replaces the one before.
void abscatalog_request_libraries(void);
void abscatalog_request_books(abs_list_t list, abs_filter_t filter, const char *filter_id, abs_sort_t sort,
							  const char *query);
void abscatalog_request_names(abs_names_t kind);
void abscatalog_request_detail(const char *item_id);

// Moves each time a request finishes. A page compares it with the one it saw.
unsigned abscatalog_serial(void);

typedef struct {
	abs_result_state_t state;
	bool offline; // the server was not reached; this is the copy kept on the card
	int loaded;
	int total;
	char error[160];
} abs_result_t;

// The answer to the last request, and its rows. The arrays belong to this
// module and stay valid until the next request is made; GUI thread only.
void abscatalog_result(abs_result_t *out);
const abs_book_t *abscatalog_books(int *count);
const abs_name_t *abscatalog_names(int *count);
const abs_library_t *abscatalog_libraries(int *count);
const abs_detail_t *abscatalog_detail(void);

// The address of a book's cover, `size` pixels square, as a JPEG. "" when
// signed out.
void abscatalog_cover_url(const char *item_id, int size, char *out, size_t out_size);

// Forgets everything kept on the card: goes with signing out.
void abscatalog_clear(void);

// True while a request is on the wire.
bool abscatalog_busy(void);

#endif /* ABSCATALOG_H */
