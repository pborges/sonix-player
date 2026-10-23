#ifndef ABSDL_H
#define ABSDL_H

#include <stdbool.h>
#include <stddef.h>

// Audiobookshelf books kept on the card.
//
// A book is downloaded into the Audiobooks folder the way the server lays its
// own files out,
//
//     <card>/Audiobooks/<author>/<series>/<title>/
//
// so the audiobook index files it under the right author and series, and the
// audiobook player plays it like any other book: chapters, speed, bookmarks,
// sleep timer. A folder of that name that is not ours gets " (ABS)" after it
// rather than having files mixed into it.
//
// Beside the audio sits .abs.json: the book's id on the server, its files, and
// where each file starts within the book -- the server keeps one position for
// the whole book, the player one per file. It is what makes a folder ours: only
// a folder with that file is ever deleted from here, and a book copied onto the
// card by hand can never be.
//
// One download at a time, on a thread of its own, from a queue. Each file is
// written as ".<name>.part" and renamed when whole; a download cut off --
// the Wi-Fi went, the player went to sleep -- picks up where it stopped. A
// book whose .abs.json says it is not complete is listed, so it can be resumed.

#define ABSDL_QUEUE_MAX 32

typedef enum {
	ABSDL_NONE,	   // not on the card
	ABSDL_QUEUED,  // waiting for the one before it
	ABSDL_RUNNING, // downloading now
	ABSDL_PARTIAL, // started and stopped: failed, cancelled, or the player was switched off
	ABSDL_DONE,
	ABSDL_DELETING,
} absdl_state_t;

typedef struct {
	char id[40];
	char title[160];
	char author[112];
	char folder[512];		// "" until the download has started
	absdl_state_t state;
	long long done, total; // bytes
	double duration;		// seconds
	char error[160];		// why the last attempt stopped, a translation key or the transport's words
} absdl_entry_t;

// Where the card is. Reads the books already downloaded. Once at startup and
// on every card change.
void absdl_set_root(const char *sd_root);

// Reads the card again, in the background: a book deleted from the Audiobooks
// pages or the file manager stops being listed as on the card. Cheap; called
// whenever an Audiobookshelf page opens.
void absdl_refresh(void);

// Adds a book to the queue. False when it is already there or on the card, or
// the queue is full.
bool absdl_enqueue(const char *item_id, const char *title, const char *author);

// Stops a book's download, or takes it off the queue. What was written stays,
// for a later resume.
void absdl_cancel(const char *item_id);

// Deletes a book from the card -- only the files this module wrote -- and from
// the audiobook index. Done on the download thread; the state is DELETING
// until it has.
bool absdl_delete(const char *item_id);

// What is known about one book. False when nothing is.
bool absdl_get(const char *item_id, absdl_entry_t *out);

// Every book on the card or in the queue: running first, then queued, then the
// rest by title. Returns how many were written.
int absdl_list(absdl_entry_t *out, int max);

// Moves whenever anything above has changed.
unsigned absdl_serial(void);

// The book a file of the card belongs to, when it is one of ours: its id on the
// server, and where `seconds` into that file falls within the whole book.
// What progress sync is built on.
bool absdl_item_for_file(const char *path, char *id_out, size_t id_size, double seconds, double *book_seconds);

// The other way round: the file of a downloaded book that holds the book's
// `book_seconds`, and the second within it.
bool absdl_file_at(const char *item_id, double book_seconds, char *file_out, size_t file_size, double *seconds_out);

// True while a download holds the network, so the Wi-Fi is not parked under it.
bool absdl_busy(void);

#endif /* ABSDL_H */
