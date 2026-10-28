#ifndef ABSSYNC_H
#define ABSSYNC_H

#include <stdbool.h>
#include <stddef.h>

// How far the user got in each downloaded Audiobookshelf book, both ways.
//
// Out: while a downloaded book plays, the time listened and the place reached
// are gathered into listening sessions -- one from each play to the pause that
// ends it. A finished session goes into a queue on the card,
// .local/abs/sessions, and the queue is sent whenever there is Wi-Fi, as the
// server's own "local sessions" (POST /api/session/local-all). That one call is
// the right one on both counts: the server counts the listening in its stats
// whatever happens, and moves the book's position only when the session is
// newer than what it has -- so an evening on the phone is not undone by an
// older session from this player arriving later. Sessions carry their own ids,
// so one sent twice after a dropped connection is counted once.
//
// In: when the server has a book further along than anything this player
// sent -- listened to on the phone -- its place is written into the audiobook
// index, so the audiobook player starts it there.
//
// The player is away from Wi-Fi most of the time. Nothing here turns it on;
// the Sync page asks.

// Where the card is. Once at startup and on every card change; the queue is
// read back from it.
void abssync_set_root(const char *sd_root);

// What is playing, about once a second, from the GUI thread. `file` is the
// loaded file, "" for none; `position` its second.
void abssync_note_playback(const char *file, bool playing, double position);

// The book loaded in the player, paused, was moved on another device since:
// the GUI thread takes the move and puts the player there. False when there is
// none.
bool abssync_take_move(char *item_out, size_t item_size, double *book_seconds);

// Send what is waiting and fetch what the server has, now. Returns at once.
void abssync_now(void);

// The book of `item_id` was marked finished, or not finished, on this player.
// Sent like a session.
void abssync_mark_finished(const char *item_id, bool finished);

// One session waiting to be sent, for the list on the Sync page.
typedef struct {
	char id[72];
	char title[160];
	double start, current; // seconds into the book
	double listened;	   // seconds of actual playing
	long long started;	   // ms since 1970; 1 when the clock was not set
	int finished;		   // -1 nothing said, 0 marked not finished, 1 finished
	bool open;			   // being listened to now: it closes at the next pause
} abssync_entry_t;

// What will go at the next sync, oldest first, the open session last. Returns
// how many were written. Reads the card: GUI thread, but not every frame.
int abssync_list(abssync_entry_t *out, int max);

// Takes one session out before it is sent: a skip to the end by mistake, a
// book left playing in a bag. The book's place then comes back from the
// server at the next sync, as if this player had never moved it. False when
// the session is not waiting any more.
bool abssync_remove(const char *id);

typedef struct {
	int pending;		// sessions waiting to be sent, the open one included
	bool sending;		// a sync is on the wire
	long long last_sync; // seconds since 1970 of the last one that worked, 0 never
	char error[160];	// why the last one did not work, "" when it did
	unsigned attempts;	// moves at the end of every attempt, worked or not
} abssync_status_t;

void abssync_get_status(abssync_status_t *out);

// Moves whenever the status above changes.
unsigned abssync_serial(void);

// True while a sync holds the network, so the Wi-Fi is not parked under it.
bool abssync_network_wanted(void);

#endif /* ABSSYNC_H */
