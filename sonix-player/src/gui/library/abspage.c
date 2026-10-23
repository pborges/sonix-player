#include "abspage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/abssettings.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/qobuzart.h"
#include "src/system/abs/abs.h"
#include "src/system/abs/abscatalog.h"
#include "src/system/abs/absdl.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/wifi.h"

#define POLL_MS 150

// The list rows: the Audiobooks page's height and thumbnail, with a second
// line under the title for the author, the length and how far the user got.
#define ROW_HEIGHT 100
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 14
#define THUMB_SIZE 72
#define NAME_ROW_HEIGHT 88
#define NAME_ROW_PITCH (NAME_ROW_HEIGHT + ROW_GAP)

// Rows that exist as widgets, windowed over the list. Six and a bit fit on the
// taller panel; ten leave room to scroll without rebinding every row. Each
// takes one of qobuzart's twelve cover slots, and the book page's big cover
// takes the last.
#define ROW_POOL 10
#define HERO_SLOT (QOBUZART_SLOTS - 1)
#define HERO_SIZE 240

static gui_config_t *g_cfg;
static const char art_owner; // qobuzart's slots are ours while one of these pages is up

static lv_timer_t *poll_timer;
static unsigned seen_serial;

typedef enum {
	WAIT_NONE,
	WAIT_LIBRARIES,
	WAIT_BOOKS,
	WAIT_NAMES,
	WAIT_DETAIL,
} wait_t;
static wait_t waiting;

static lv_obj_t *home_screen;
static lv_obj_t *books_screen;
static lv_obj_t *names_screen;
static lv_obj_t *detail_screen;
static lv_obj_t *search_screen;
static lv_obj_t *downloads_screen;
static unsigned seen_downloads;

static void books_open(abs_list_t list, abs_filter_t filter, const char *filter_id, const char *title,
					   const char *query);
static void names_open(abs_names_t kind);
static void detail_open(const abs_book_t *book);
static void downloads_open(void);

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// "12h 05m", "45m": a book's length or what is left of it.
static void format_length(double seconds, char *out, size_t size) {
	int minutes = (int)(seconds / 60.0 + 0.5);
	if (minutes >= 60) {
		snprintf(out, size, "%dh %02dm", minutes / 60, minutes % 60);
	} else {
		snprintf(out, size, "%dm", minutes);
	}
}

// The second line of a book's row: author, length, and the user's place in it.
static void book_subtitle(const abs_book_t *b, char *out, size_t size) {
	char length[32] = "";
	if (b->duration > 0) {
		format_length(b->duration, length, sizeof(length));
	}
	char place[48] = "";
	if (b->finished) {
		snprintf(place, sizeof(place), "%s", tr("abs_finished"));
	} else if (b->started && b->progress > 0) {
		snprintf(place, sizeof(place), "%d%%", (int)(b->progress * 100.0));
	}
	absdl_entry_t d;
	if (absdl_get(b->id, &d) && (d.state == ABSDL_DONE || d.state == ABSDL_RUNNING || d.state == ABSDL_QUEUED)) {
		size_t n = strlen(place);
		snprintf(place + n, sizeof(place) - n, "%s%s", place[0] ? " · " : "",
				 tr(d.state == ABSDL_DONE ? "abs_on_card" : "abs_downloading_short"));
	}
	snprintf(out, size, "%s%s%s%s%s", b->author, b->author[0] && length[0] ? " · " : "", length,
			 (b->author[0] || length[0]) && place[0] ? " · " : "", place);
}

// "412 MB", "1.3 GB".
static void format_size(long long bytes, char *out, size_t size) {
	if (bytes >= 1000LL * 1000 * 1000) {
		snprintf(out, size, "%.1f GB", (double)bytes / 1e9);
	} else {
		snprintf(out, size, "%lld MB", (bytes + 500000) / 1000000);
	}
}

static bool online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0];
}

// A downloaded book handed to the audiobook player, where it was left.
static void play_downloaded(const char *id) {
	char file[900], book[900];
	double seconds;
	if (!absdl_file_at(id, 0, file, sizeof(file), &seconds) || !audiobookdb_book_for_file(file, book, sizeof(book))) {
		toast_error("abs_play_not_indexed");
		return;
	}
	audiobooks_play_book(book);
}

static void start_download(const char *id, const char *title, const char *author) {
	if (!online()) {
		toast_error("enable_wi_fi_first");
		return;
	}
	if (!absdl_enqueue(id, title, author)) {
		toast_error("abs_queue_full");
	}
}

// What a download is doing, for a line under the book: "Downloading 45%",
// "On the card · 412 MB", "Stopped: <why>".
static void download_status(const absdl_entry_t *d, char *out, size_t size) {
	char total[32];
	format_size(d->total, total, sizeof(total));
	switch (d->state) {
	case ABSDL_QUEUED:
		snprintf(out, size, "%s", tr("abs_waiting"));
		break;
	case ABSDL_RUNNING:
		snprintf(out, size, tr("abs_downloading"), d->total > 0 ? (int)(d->done * 100 / d->total) : 0);
		break;
	case ABSDL_DONE:
		snprintf(out, size, "%s · %s", tr("abs_on_card"), total);
		break;
	case ABSDL_PARTIAL:
		if (d->error[0]) {
			snprintf(out, size, "%s: %s", tr("abs_stopped"), tr(d->error));
		} else {
			snprintf(out, size, "%s", tr("abs_stopped"));
		}
		break;
	case ABSDL_DELETING:
		snprintf(out, size, "%s", tr("abs_deleting"));
		break;
	case ABSDL_NONE:
		out[0] = '\0';
		break;
	}
}

// A failure as the user should read it: the keys this module and the catalogue
// use, else the transport's own words.
static const char *error_text(const char *error) { return error[0] ? tr(error) : tr("abs_no_reply"); }

static lv_obj_t *corner_button(lv_obj_t *screen, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -g_cfg->padding - slot * (56 + 6), g_cfg->padding + g_cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

// A scrollable page under the title with a fixed-height canvas in it, so the
// row pool can stand in for any number of entries.
static lv_obj_t *make_viewport(lv_obj_t *screen, lv_obj_t **body_out, lv_obj_t **empty_out, lv_event_cb_t scroll_cb) {
	int content_top = settingsrow_content_top(g_cfg);
	int width = (int)g_cfg->screen_width - 2 * g_cfg->padding;

	lv_obj_t *list = lv_obj_create(screen);
	lv_obj_set_size(list, lv_pct(100), (int)g_cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, g_cfg->padding, 0);
	lv_obj_set_style_pad_ver(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_set_event_bubble(list, true);

	lv_obj_t *body = lv_obj_create(list);
	lv_obj_set_width(body, width);
	lv_obj_set_height(body, 1);
	lv_obj_set_pos(body, 0, 0);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_event_bubble(body, true);

	lv_obj_t *empty = lv_label_create(list);
	lv_obj_set_width(empty, width);
	lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty, &font_ui_24, 0);
	lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_set_hidden(empty, true);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);
	*body_out = body;
	*empty_out = empty;
	return list;
}

static lv_obj_t *make_row_button(lv_obj_t *parent, int height, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, (int)g_cfg->screen_width - 2 * g_cfg->padding, height);
	lv_obj_set_x(button, 0);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, ROW_PAD, 0);
	lv_obj_set_style_pad_column(button, ROW_PAD, 0);
	lv_obj_set_hidden(button, true);
	lv_obj_set_event_bubble(button, true);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return button;
}

static bool tapped(void) { return !player_sheet_drag_active() && !switcher_back_drag_active(); }

// ---------------------------------------------------------------------------
// the home page
// ---------------------------------------------------------------------------

static lv_obj_t *expired_row;
static lv_obj_t *library_value;
static abs_library_t libraries[ABS_MAX_LIBRARIES];
static int library_count;

static bool library_known(void) {
	char id[ABS_ID_MAX];
	abscatalog_library(id, sizeof(id), NULL, 0);
	return id[0] != '\0';
}

static void home_refresh(void) {
	abs_status_t st;
	abs_get_status(&st);
	lv_obj_set_hidden(expired_row, st.state != ABS_STATE_EXPIRED);

	char name[ABS_NAME_MAX];
	abscatalog_library(NULL, 0, name, sizeof(name));
	lv_label_set_text(library_value, name[0] ? name : tr("loading"));
}

// The libraries arrived: keep the one picked if the server still has it,
// else take its first.
static void libraries_arrived(const abs_result_t *res) {
	int count;
	const abs_library_t *got = abscatalog_libraries(&count);
	if (res->state == ABS_RESULT_READY && count > 0) {
		library_count = count;
		memcpy(libraries, got, (size_t)count * sizeof(*got));
		char id[ABS_ID_MAX];
		abscatalog_library(id, sizeof(id), NULL, 0);
		int keep = 0;
		for (int i = 0; i < count; i++) {
			if (strcmp(libraries[i].id, id) == 0) {
				keep = i;
			}
		}
		abscatalog_set_library(libraries[keep].id, libraries[keep].name);
	} else if (res->state == ABS_RESULT_FAILED && !library_known()) {
		toast_error(error_text(res->error));
	}
	home_refresh();
}

static void library_picked(void *user) {
	int i = (int)(intptr_t)user;
	if (i >= 0 && i < library_count) {
		abscatalog_set_library(libraries[i].id, libraries[i].name);
		home_refresh();
	}
}

static void library_cb(lv_event_t *e) {
	if (library_count < 2) {
		return;
	}
	char id[ABS_ID_MAX];
	abscatalog_library(id, sizeof(id), NULL, 0);
	popover_item_t items[ABS_MAX_LIBRARIES];
	int n = library_count < 7 ? library_count : 7; // what the popover shows
	for (int i = 0; i < n; i++) {
		items[i] = (popover_item_t){.label = libraries[i].name,
									.action = library_picked,
									.user = (void *)(intptr_t)i,
									.checked = strcmp(libraries[i].id, id) == 0};
	}
	popover_show(lv_event_get_current_target(e), items, n);
}

// The lists need to know which library they are of.
static bool library_ready(void) {
	if (!library_known()) {
		toast_error("loading");
		return false;
	}
	return true;
}

static void continue_cb(lv_event_t *e) {
	(void)e;
	books_open(ABS_LIST_CONTINUE, ABS_FILTER_NONE, NULL, tr("abs_continue"), NULL);
}

static void all_cb(lv_event_t *e) {
	(void)e;
	if (library_ready()) {
		books_open(ABS_LIST_BOOKS, ABS_FILTER_NONE, NULL, tr("abs_all_books"), NULL);
	}
}

static void series_cb(lv_event_t *e) {
	(void)e;
	if (library_ready()) {
		names_open(ABS_NAMES_SERIES);
	}
}

static void authors_cb(lv_event_t *e) {
	(void)e;
	if (library_ready()) {
		names_open(ABS_NAMES_AUTHORS);
	}
}

static void search_cb(lv_event_t *e);

static void downloads_row_cb(lv_event_t *e) {
	(void)e;
	downloads_open();
}

static void expired_cb(lv_event_t *e) {
	(void)e;
	switch_screen(abssettings_screen());
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	home_refresh();
	// Every visit: a library renamed or added on the server shows up here.
	waiting = WAIT_LIBRARIES;
	abscatalog_request_libraries();
}

static void build_home(void) {
	home_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(home_screen, g_cfg, "audiobookshelf");

	// Only when the server has signed the player out: the way to the account,
	// which lives in the Audiobooks options and nowhere here.
	expired_row = settingsrow_action(container, "abs_expired", expired_cb, NULL);
	lv_obj_set_style_text_color(settingsrow_name_label(expired_row), lv_color_make(232, 96, 80), 0);

	settingsrow_add(container, "abs_library", &library_value, library_cb, NULL);
	lv_label_set_long_mode(library_value, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(library_value, lv_pct(60), 0);
	lv_obj_set_height(library_value, lv_font_get_line_height(&font_ui_24));

	settingsrow_add(container, "abs_continue", NULL, continue_cb, NULL);
	settingsrow_add(container, "abs_downloads", NULL, downloads_row_cb, NULL);
	settingsrow_add(container, "abs_all_books", NULL, all_cb, NULL);
	settingsrow_add(container, "audiobook_series", NULL, series_cb, NULL);
	settingsrow_add(container, "audiobook_authors", NULL, authors_cb, NULL);
	settingsrow_add(container, "abs_search", NULL, search_cb, NULL);

	lv_obj_add_event_cb(home_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(home_screen);
}

// ---------------------------------------------------------------------------
// the books page
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *subtitle;
	int index;

	bool art_requested;
	bool art_settled;
	bool has_art;
	cover_image_t art;
} book_row_t;

static lv_obj_t *books_title;
static lv_obj_t *books_sort_btn;
static lv_obj_t *books_sort_icon;
static lv_obj_t *books_list;
static lv_obj_t *books_body;
static lv_obj_t *books_empty;
static book_row_t book_rows[ROW_POOL];

static abs_book_t *books; // the page's own copy: the catalogue moves on to other requests
static int book_count;

static abs_list_t books_kind;
static abs_filter_t books_filter;
static char books_filter_id[ABS_ID_MAX];
static char books_query[128];

// The orders each list offers, and where its choice is kept.
typedef struct {
	const char *config_key;
	int count;
	abs_sort_t choices[3];
} sorts_t;

static const sorts_t *books_sorts(void) {
	static const sorts_t ALL = {"sort_all", 3, {ABS_SORT_TITLE, ABS_SORT_AUTHOR, ABS_SORT_ADDED}};
	static const sorts_t AUTHOR = {"sort_author", 3, {ABS_SORT_TITLE, ABS_SORT_SERIES, ABS_SORT_ADDED}};
	static const sorts_t SERIES = {"sort_series", 2, {ABS_SORT_SERIES, ABS_SORT_TITLE}};
	if (books_kind != ABS_LIST_BOOKS) {
		return NULL; // Continue and a search come in the server's own order
	}
	return books_filter == ABS_FILTER_SERIES ? &SERIES : books_filter == ABS_FILTER_AUTHOR ? &AUTHOR : &ALL;
}

static abs_sort_t books_sort(void) {
	const sorts_t *s = books_sorts();
	if (!s) {
		return ABS_SORT_TITLE;
	}
	long v = config_get_int("audiobookshelf", s->config_key, s->choices[0]);
	for (int i = 0; i < s->count; i++) {
		if (s->choices[i] == v) {
			return (abs_sort_t)v;
		}
	}
	return s->choices[0];
}

static const char *sort_label(abs_sort_t sort) {
	switch (sort) {
	case ABS_SORT_AUTHOR:
		return "abs_sort_author";
	case ABS_SORT_ADDED:
		return "medialist_sort_added_new";
	case ABS_SORT_SERIES:
		return "audiobook_sort_series";
	default:
		return "medialist_sort_name_az";
	}
}

static const lv_image_dsc_t *sort_icon(abs_sort_t sort) {
	switch (sort) {
	case ABS_SORT_ADDED:
		return &icon_sort_date_new;
	case ABS_SORT_SERIES:
		return &icon_sort_series;
	default:
		return &icon_sort_az;
	}
}

static void row_show_glyph(book_row_t *row) {
	lv_image_set_src(row->icon, &icon_book_headphones_row);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

static void row_drop_art(book_row_t *row) {
	qobuzart_release(&art_owner, (int)(row - book_rows));
	if (row->has_art) {
		row_show_glyph(row);
		cover_free(&row->art);
		row->has_art = false;
	}
	row->art_requested = false;
	row->art_settled = false;
}

static void book_row_bind(book_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row_drop_art(row);
	row->index = index;
	if (index < 0 || index >= book_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}
	const abs_book_t *b = &books[index];
	char sub[300];
	book_subtitle(b, sub, sizeof(sub));
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->title, b->title);
	lv_label_set_text(row->subtitle, sub);
	row_show_glyph(row);
	row->art_settled = !b->has_cover;
}

// Asks for the covers of the rows on screen and puts up what has arrived. True
// while some are still on their way.
static bool books_art_update(void) {
	bool pending = false;
	for (int i = 0; i < ROW_POOL; i++) {
		book_row_t *row = &book_rows[i];
		if (row->index < 0 || row->index >= book_count || row->art_settled) {
			continue;
		}
		if (!row->art_requested) {
			char url[QOBUZART_URL_MAX];
			abscatalog_cover_url(books[row->index].id, THUMB_SIZE, url, sizeof(url));
			qobuzart_request(&art_owner, i, url, THUMB_SIZE);
			row->art_requested = true;
		}
		bool finished = false;
		cover_image_t image;
		if (qobuzart_take(&art_owner, i, &image, &finished)) {
			row->art = image;
			row->has_art = true;
			row->art_settled = true;
			lv_image_set_src(row->icon, &row->art.dsc);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
		} else if (finished) {
			row->art_settled = true;
		} else {
			pending = true;
		}
	}
	return pending;
}

static void books_window_update(void) {
	int scroll = lv_obj_get_scroll_y(books_list);
	int first = (scroll < 0 ? 0 : scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > book_count) {
		first = book_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	// index -> row index % ROW_POOL, so a one-step slide rebinds one row.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		book_row_bind(&book_rows[index % ROW_POOL], index < book_count ? index : -1);
	}
	books_art_update();
}

static void books_scroll_cb(lv_event_t *e) {
	(void)e;
	books_window_update();
}

static void books_set_rows(const abs_book_t *rows, int count) {
	for (int i = 0; i < ROW_POOL; i++) {
		book_row_bind(&book_rows[i], -1);
		book_rows[i].index = -1;
	}
	free(books);
	books = NULL;
	book_count = 0;
	if (count > 0 && (books = malloc((size_t)count * sizeof(*books)))) {
		memcpy(books, rows, (size_t)count * sizeof(*books));
		book_count = count;
	}
	lv_obj_set_height(books_body, book_count > 0 ? book_count * ROW_PITCH : 1);
	books_window_update();
}

static void books_show_message(const char *text) {
	lv_label_set_text(books_empty, text);
	lv_obj_set_hidden(books_empty, text == NULL || !text[0]);
}

static void books_request(void) {
	waiting = WAIT_BOOKS;
	abscatalog_request_books(books_kind, books_filter, books_filter_id, books_sort(), books_query);
	books_set_rows(NULL, 0);
	books_show_message(tr("loading"));
}

static void books_arrived(const abs_result_t *res) {
	if (res->state == ABS_RESULT_LOADING) {
		if (res->total > 0) {
			char text[96];
			snprintf(text, sizeof(text), tr("abs_loading_count"), res->loaded, res->total);
			books_show_message(text);
		}
		return;
	}
	if (res->state == ABS_RESULT_FAILED) {
		books_show_message(error_text(res->error));
		return;
	}
	int count;
	const abs_book_t *rows = abscatalog_books(&count);
	books_set_rows(rows, count);
	books_show_message(count > 0								? NULL
					   : books_kind == ABS_LIST_CONTINUE		? tr("abs_continue_empty")
					   : books_kind == ABS_LIST_SEARCH			? tr("no_results")
																: tr("abs_no_books"));
	if (res->offline) {
		toast_error("abs_offline_list");
	}
}

static void sort_picked(void *user) {
	const sorts_t *s = books_sorts();
	if (!s) {
		return;
	}
	config_set_int("audiobookshelf", s->config_key, (long)(intptr_t)user);
	config_save();
	lv_image_set_src(books_sort_icon, sort_icon(books_sort()));
	books_request();
}

static void books_sort_cb(lv_event_t *e) {
	const sorts_t *s = books_sorts();
	if (!s) {
		return;
	}
	abs_sort_t now = books_sort();
	popover_item_t items[3];
	for (int i = 0; i < s->count; i++) {
		items[i] = (popover_item_t){.label = tr(sort_label(s->choices[i])),
									.action = sort_picked,
									.user = (void *)(intptr_t)s->choices[i],
									.checked = s->choices[i] == now};
	}
	popover_show(lv_event_get_current_target(e), items, s->count);
}

static void book_clicked_cb(lv_event_t *e) {
	if (!tapped()) {
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (book_rows[i].button == button && book_rows[i].index >= 0 && book_rows[i].index < book_count) {
			detail_open(&books[book_rows[i].index]);
			return;
		}
	}
}

static void books_open(abs_list_t list, abs_filter_t filter, const char *filter_id, const char *title,
					   const char *query) {
	books_kind = list;
	books_filter = filter;
	snprintf(books_filter_id, sizeof(books_filter_id), "%s", filter_id ? filter_id : "");
	snprintf(books_query, sizeof(books_query), "%s", query ? query : "");
	lv_label_set_text(books_title, title);
	lv_obj_set_hidden(books_sort_btn, books_sorts() == NULL);
	lv_image_set_src(books_sort_icon, sort_icon(books_sort()));
	lv_obj_scroll_to_y(books_list, 0, LV_ANIM_OFF);
	books_request();
	switch_screen(books_screen);
}

// A download started or finished: the rows say so.
static void books_rebind(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		int index = book_rows[i].index;
		if (index >= 0 && index < book_count) {
			char sub[300];
			book_subtitle(&books[index], sub, sizeof(sub));
			lv_label_set_text(book_rows[i].subtitle, sub);
		}
	}
}

static void books_loaded_cb(lv_event_t *e) {
	(void)e;
	books_rebind();
	// Back from a book's page: the covers were handed to its big picture.
	for (int i = 0; i < ROW_POOL; i++) {
		if (!book_rows[i].has_art) {
			book_rows[i].art_requested = false;
			book_rows[i].art_settled = book_rows[i].index < 0 || book_rows[i].index >= book_count ||
									   !books[book_rows[i].index].has_cover;
		}
	}
	books_art_update();
}

static void build_books(void) {
	books_screen = lv_obj_create(NULL);
	lv_obj_add_style(books_screen, &theme_style_screen, 0);
	books_title = settingsrow_title(books_screen, g_cfg, "audiobookshelf");
	settingsrow_title_corner_slots(books_title, g_cfg, 1);
	books_sort_btn = corner_button(books_screen, 0, &icon_sort_az, books_sort_cb);
	books_sort_icon = lv_obj_get_child(books_sort_btn, 0);
	books_list = make_viewport(books_screen, &books_body, &books_empty, books_scroll_cb);

	for (int i = 0; i < ROW_POOL; i++) {
		book_row_t *row = &book_rows[i];
		row->button = make_row_button(books_body, ROW_HEIGHT, book_clicked_cb);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		lv_obj_t *text = lv_obj_create(row->button);
		lv_obj_set_flex_grow(text, 1);
		lv_obj_set_height(text, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(text, 0, 0);
		lv_obj_set_style_border_width(text, 0, 0);
		lv_obj_set_style_pad_all(text, 0, 0);
		lv_obj_set_style_pad_row(text, 2, 0);
		lv_obj_set_scrollable(text, false);
		lv_obj_set_clickable(text, false);
		lv_obj_set_event_bubble(text, true);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);

		row->title = lv_label_create(text);
		lv_obj_set_width(row->title, lv_pct(100));
		lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
		lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->title, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->title, &font_ui_24, 0);

		row->subtitle = lv_label_create(text);
		lv_obj_set_width(row->subtitle, lv_pct(100));
		lv_label_set_long_mode(row->subtitle, LV_LABEL_LONG_DOT);
		lv_obj_set_height(row->subtitle, lv_font_get_line_height(&font_ui_20));
		lv_obj_add_style(row->subtitle, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->subtitle, &font_ui_20, 0);

		row->index = -1;
	}

	player_sheet_attach_drag(books_screen, true);
	switcher_attach_back_gesture(books_screen);
	lv_obj_add_event_cb(books_screen, books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the series and authors page
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *button;
	lv_obj_t *label;
	lv_obj_t *count;
	int index;
} name_row_t;

static lv_obj_t *names_title;
static lv_obj_t *names_list;
static lv_obj_t *names_body;
static lv_obj_t *names_empty;
static name_row_t name_rows[ROW_POOL];
static abs_name_t *names;
static int name_count;
static abs_names_t names_kind;

static void name_row_bind(name_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row->index = index;
	if (index < 0 || index >= name_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * NAME_ROW_PITCH);
	lv_label_set_text(row->label, names[index].name);
	lv_label_set_text_fmt(row->count, "%d", names[index].books);
}

static void names_window_update(void) {
	int scroll = lv_obj_get_scroll_y(names_list);
	int first = (scroll < 0 ? 0 : scroll / NAME_ROW_PITCH) - 1;
	if (first + ROW_POOL > name_count) {
		first = name_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		name_row_bind(&name_rows[index % ROW_POOL], index < name_count ? index : -1);
	}
}

static void names_scroll_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void names_show_message(const char *text) {
	lv_label_set_text(names_empty, text ? text : "");
	lv_obj_set_hidden(names_empty, text == NULL || !text[0]);
}

static void names_set_rows(const abs_name_t *rows, int count) {
	for (int i = 0; i < ROW_POOL; i++) {
		name_row_bind(&name_rows[i], -1);
		name_rows[i].index = -1;
	}
	free(names);
	names = NULL;
	name_count = 0;
	if (count > 0 && (names = malloc((size_t)count * sizeof(*names)))) {
		memcpy(names, rows, (size_t)count * sizeof(*names));
		name_count = count;
	}
	lv_obj_set_height(names_body, name_count > 0 ? name_count * NAME_ROW_PITCH : 1);
	names_window_update();
}

static void names_arrived(const abs_result_t *res) {
	if (res->state == ABS_RESULT_LOADING) {
		if (res->total > 0) {
			char text[96];
			snprintf(text, sizeof(text), tr("abs_loading_count"), res->loaded, res->total);
			names_show_message(text);
		}
		return;
	}
	if (res->state == ABS_RESULT_FAILED) {
		names_show_message(error_text(res->error));
		return;
	}
	int count;
	const abs_name_t *rows = abscatalog_names(&count);
	names_set_rows(rows, count);
	names_show_message(count > 0 ? NULL : tr("no_results"));
	if (res->offline) {
		toast_error("abs_offline_list");
	}
}

static void name_clicked_cb(lv_event_t *e) {
	if (!tapped()) {
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		int index = name_rows[i].index;
		if (name_rows[i].button == button && index >= 0 && index < name_count) {
			books_open(ABS_LIST_BOOKS, names_kind == ABS_NAMES_SERIES ? ABS_FILTER_SERIES : ABS_FILTER_AUTHOR,
					   names[index].id, names[index].name, NULL);
			return;
		}
	}
}

static void names_open(abs_names_t kind) {
	names_kind = kind;
	lv_label_set_text(names_title, tr(kind == ABS_NAMES_SERIES ? "audiobook_series" : "audiobook_authors"));
	names_set_rows(NULL, 0);
	names_show_message(tr("loading"));
	lv_obj_scroll_to_y(names_list, 0, LV_ANIM_OFF);
	waiting = WAIT_NAMES;
	abscatalog_request_names(kind);
	switch_screen(names_screen);
}

static void build_names(void) {
	names_screen = lv_obj_create(NULL);
	lv_obj_add_style(names_screen, &theme_style_screen, 0);
	names_title = settingsrow_title(names_screen, g_cfg, "audiobook_series");
	names_list = make_viewport(names_screen, &names_body, &names_empty, names_scroll_cb);

	for (int i = 0; i < ROW_POOL; i++) {
		name_row_t *row = &name_rows[i];
		row->button = make_row_button(names_body, NAME_ROW_HEIGHT, name_clicked_cb);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_set_height(row->label, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->count = lv_label_create(row->button);
		lv_obj_add_style(row->count, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->count, &font_ui_22, 0);
		row->index = -1;
	}

	player_sheet_attach_drag(names_screen, true);
	switcher_attach_back_gesture(names_screen);
}

// ---------------------------------------------------------------------------
// a book's page
// ---------------------------------------------------------------------------

static lv_obj_t *detail_scroll;
static lv_obj_t *detail_cover;
static lv_obj_t *detail_name;
static lv_obj_t *detail_author;
static lv_obj_t *detail_facts;
static lv_obj_t *detail_place;
static lv_obj_t *detail_text;
static lv_obj_t *detail_status;
static lv_obj_t *detail_primary;
static lv_obj_t *detail_primary_label;
static lv_obj_t *detail_secondary;
static char detail_id[ABS_ID_MAX];
static char detail_title_text[ABS_TITLE_MAX];
static char detail_author_text[ABS_NAME_MAX];
static bool detail_art_settled;
static bool detail_has_art;
static cover_image_t detail_art;

static void detail_drop_art(void) {
	qobuzart_release(&art_owner, HERO_SLOT);
	if (detail_has_art) {
		lv_image_set_src(detail_cover, &icon_book_headphones);
		lv_obj_set_style_image_recolor_opa(detail_cover, LV_OPA_COVER, 0);
		cover_free(&detail_art);
		detail_has_art = false;
	}
}

static bool detail_art_update(void) {
	if (detail_art_settled) {
		return false;
	}
	bool finished = false;
	cover_image_t image;
	if (qobuzart_take(&art_owner, HERO_SLOT, &image, &finished)) {
		detail_art = image;
		detail_has_art = true;
		detail_art_settled = true;
		lv_image_set_src(detail_cover, &detail_art.dsc);
		lv_obj_set_style_image_recolor_opa(detail_cover, LV_OPA_TRANSP, 0);
		return false;
	}
	if (finished) {
		detail_art_settled = true;
	}
	return !finished;
}

// The lines that the list row already knew, filled in at once; the rest when
// the server answers.
static void detail_show(const abs_book_t *b, const abs_detail_t *d) {
	lv_label_set_text(detail_name, b->title);
	lv_label_set_text(detail_author, b->author);

	char facts[400] = "", length[32] = "";
	if (b->duration > 0) {
		format_length(b->duration, length, sizeof(length));
	}
	if (d) {
		char narrator[ABS_NAME_MAX + 32] = "";
		if (d->narrator[0]) {
			snprintf(narrator, sizeof(narrator), tr("abs_read_by"), d->narrator);
		}
		snprintf(facts, sizeof(facts), "%s%s%s%s%s", narrator, narrator[0] ? "\n" : "", b->series,
				 b->series[0] ? "\n" : "", length);
		if (d->chapters > 0) {
			char chapters[48];
			snprintf(chapters, sizeof(chapters), tr("abs_chapters"), d->chapters);
			size_t n = strlen(facts);
			snprintf(facts + n, sizeof(facts) - n, "%s%s", length[0] ? " · " : "", chapters);
		}
		if (d->published[0]) {
			size_t n = strlen(facts);
			snprintf(facts + n, sizeof(facts) - n, " · %s", d->published);
		}
	} else {
		snprintf(facts, sizeof(facts), "%s%s%s", b->series, b->series[0] ? "\n" : "", length);
	}
	lv_label_set_text(detail_facts, facts);

	char place[96] = "";
	if (b->finished) {
		snprintf(place, sizeof(place), "%s", tr("abs_finished"));
	} else if (b->started && b->duration > 0) {
		char left[32];
		double at = d ? d->current_time : b->progress * b->duration;
		format_length(b->duration - at, left, sizeof(left));
		snprintf(place, sizeof(place), tr("abs_progress_left"), (int)(b->progress * 100.0), left);
	} else {
		snprintf(place, sizeof(place), "%s", tr("abs_not_started"));
	}
	lv_label_set_text(detail_place, place);
	lv_label_set_text(detail_text, d && d->description ? d->description : "");
}

typedef enum {
	ACTION_NONE,
	ACTION_DOWNLOAD,
	ACTION_RESUME,
	ACTION_STOP,
	ACTION_PLAY,
} action_t;
static action_t detail_action;

// The buttons under a book: Download; Stop while it downloads; Play and Delete
// once it is on the card; Resume and Delete when it stopped halfway.
static void detail_actions_refresh(void) {
	absdl_entry_t d;
	bool known = absdl_get(detail_id, &d);
	absdl_state_t state = known ? d.state : ABSDL_NONE;

	char status[240] = "";
	if (known) {
		download_status(&d, status, sizeof(status));
	}
	lv_label_set_text(detail_status, status);
	lv_obj_set_hidden(detail_status, !status[0]);

	switch (state) {
	case ABSDL_NONE:
		detail_action = ACTION_DOWNLOAD;
		break;
	case ABSDL_QUEUED:
	case ABSDL_RUNNING:
		detail_action = ACTION_STOP;
		break;
	case ABSDL_DONE:
		detail_action = ACTION_PLAY;
		break;
	case ABSDL_PARTIAL:
		detail_action = ACTION_RESUME;
		break;
	case ABSDL_DELETING:
		detail_action = ACTION_NONE;
		break;
	}
	static const char *const LABELS[] = {"", "abs_download", "abs_resume_download", "abs_stop_download", "abs_play"};
	lv_label_set_text(detail_primary_label, tr(LABELS[detail_action]));
	lv_obj_set_hidden(detail_primary, detail_action == ACTION_NONE);
	lv_obj_set_hidden(detail_secondary, !(state == ABSDL_DONE || (state == ABSDL_PARTIAL && d.folder[0])));
}

static void detail_primary_cb(lv_event_t *e) {
	(void)e;
	switch (detail_action) {
	case ACTION_DOWNLOAD:
	case ACTION_RESUME:
		start_download(detail_id, detail_title_text, detail_author_text);
		break;
	case ACTION_STOP:
		absdl_cancel(detail_id);
		break;
	case ACTION_PLAY:
		play_downloaded(detail_id);
		break;
	case ACTION_NONE:
		break;
	}
	detail_actions_refresh();
}

static char delete_id[ABS_ID_MAX];

static void delete_confirmed(void *user) {
	(void)user;
	absdl_delete(delete_id);
}

static void ask_delete(const char *id, const char *title) {
	snprintf(delete_id, sizeof(delete_id), "%s", id);
	char message[300];
	snprintf(message, sizeof(message), tr("abs_delete_note"), title);
	confirm_show("abs_delete_download", message, "delete", delete_confirmed, NULL);
}

static void detail_secondary_cb(lv_event_t *e) {
	(void)e;
	ask_delete(detail_id, detail_title_text);
}

static void detail_arrived(const abs_result_t *res) {
	if (res->state == ABS_RESULT_FAILED) {
		lv_label_set_text(detail_text, error_text(res->error));
		return;
	}
	const abs_detail_t *d = abscatalog_detail();
	if (res->state == ABS_RESULT_READY && d && strcmp(d->book.id, detail_id) == 0) {
		detail_show(&d->book, d);
	}
}

static void detail_open(const abs_book_t *book) {
	snprintf(detail_id, sizeof(detail_id), "%s", book->id);
	snprintf(detail_title_text, sizeof(detail_title_text), "%s", book->title);
	snprintf(detail_author_text, sizeof(detail_author_text), "%s", book->author);
	detail_actions_refresh();
	detail_show(book, NULL);
	lv_obj_scroll_to_y(detail_scroll, 0, LV_ANIM_OFF);

	detail_drop_art();
	detail_art_settled = !book->has_cover;
	if (book->has_cover) {
		char url[QOBUZART_URL_MAX];
		abscatalog_cover_url(book->id, HERO_SIZE, url, sizeof(url));
		qobuzart_request(&art_owner, HERO_SLOT, url, HERO_SIZE);
	}
	waiting = WAIT_DETAIL;
	abscatalog_request_detail(book->id);
	switch_screen(detail_screen);
}

static lv_obj_t *detail_label(lv_obj_t *parent, const lv_font_t *font, bool dim, bool centred) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	if (centred) {
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	}
	lv_label_set_text(label, "");
	return label;
}

static void build_detail(void) {
	detail_screen = lv_obj_create(NULL);
	lv_obj_add_style(detail_screen, &theme_style_screen, 0);
	settingsrow_title(detail_screen, g_cfg, "audiobookshelf");

	int top = settingsrow_content_top(g_cfg);
	detail_scroll = lv_obj_create(detail_screen);
	lv_obj_set_size(detail_scroll, lv_pct(100), (int)g_cfg->screen_height - top);
	lv_obj_align(detail_scroll, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(detail_scroll, 0, 0);
	lv_obj_set_style_border_width(detail_scroll, 0, 0);
	lv_obj_set_style_radius(detail_scroll, 0, 0);
	lv_obj_set_style_pad_hor(detail_scroll, g_cfg->padding, 0);
	lv_obj_set_style_pad_bottom(detail_scroll, g_cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(detail_scroll, 10, 0);
	lv_obj_set_scroll_dir(detail_scroll, LV_DIR_VER);
	lv_obj_set_flex_flow(detail_scroll, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(detail_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(detail_scroll, true);

	detail_cover = lv_image_create(detail_scroll);
	lv_obj_set_size(detail_cover, HERO_SIZE, HERO_SIZE);
	lv_image_set_inner_align(detail_cover, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_src(detail_cover, &icon_book_headphones);
	lv_obj_add_style(detail_cover, &theme_style_icon, 0);

	detail_name = detail_label(detail_scroll, &font_ui_28, false, true);
	detail_author = detail_label(detail_scroll, &font_ui_24, true, true);
	detail_facts = detail_label(detail_scroll, &font_ui_20, true, true);
	detail_place = detail_label(detail_scroll, &font_ui_22_bold, false, true);
	lv_obj_set_style_text_color(detail_place, theme()->accent, 0);
	detail_status = detail_label(detail_scroll, &font_ui_20, true, true);

	lv_obj_t *buttons = lv_obj_create(detail_scroll);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(buttons, 0, 0);
	lv_obj_set_style_border_width(buttons, 0, 0);
	lv_obj_set_style_pad_all(buttons, 0, 0);
	lv_obj_set_style_pad_column(buttons, 12, 0);
	lv_obj_set_scrollable(buttons, false);
	lv_obj_set_event_bubble(buttons, true);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	detail_primary = lv_btn_create(buttons);
	lv_obj_set_size(detail_primary, 200, 60);
	lv_obj_add_style(detail_primary, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(detail_primary, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(detail_primary, 0, 0);
	lv_obj_add_event_cb(detail_primary, detail_primary_cb, LV_EVENT_CLICKED, NULL);
	detail_primary_label = lv_label_create(detail_primary);
	lv_obj_set_style_text_font(detail_primary_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(detail_primary_label, lv_color_white(), 0);
	lv_obj_center(detail_primary_label);

	detail_secondary = lv_btn_create(buttons);
	lv_obj_set_size(detail_secondary, 140, 60);
	lv_obj_add_style(detail_secondary, &theme_style_card, 0);
	lv_obj_set_style_radius(detail_secondary, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(detail_secondary, 0, 0);
	lv_obj_add_event_cb(detail_secondary, detail_secondary_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *secondary_label = lv_label_create(detail_secondary);
	lv_label_set_text(secondary_label, tr("delete"));
	lv_obj_add_style(secondary_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(secondary_label, &font_ui_24, 0);
	lv_obj_center(secondary_label);

	detail_text = detail_label(detail_scroll, &font_ui_20, false, false);
	lv_obj_set_style_pad_top(detail_text, 10, 0);

	player_sheet_attach_drag(detail_scroll, true);
	switcher_attach_back_gesture(detail_scroll);
	switcher_attach_back_gesture(detail_screen);
}

// ---------------------------------------------------------------------------
// the search page
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	char title[160];
	snprintf(title, sizeof(title), "“%s”", text);
	books_open(ABS_LIST_SEARCH, ABS_FILTER_NONE, NULL, title, text);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	if (!library_ready()) {
		return;
	}
	keyboard_reset(search_keyboard);
	keyboard_set_field(search_keyboard, search_field);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	keyboard_show_caret(search_field, true);
	switch_screen(search_screen);
}

static void build_search(void) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, g_cfg, "abs_search");
	int top = settingsrow_content_top(g_cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("abs_search_placeholder"));
	lv_obj_set_size(search_field, (int)g_cfg->screen_width - 2 * g_cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, g_cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);

	search_keyboard = keyboard_create(search_screen, (int)g_cfg->screen_width, 316, search_field, &icon_search, NULL,
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// the downloads page
//
// A handful of books, not a library: the rows are simply rebuilt whenever the
// downloads move on.
// ---------------------------------------------------------------------------

#define DOWNLOADS_SHOWN 64

static lv_obj_t *downloads_container;
static lv_obj_t *downloads_empty;
static absdl_entry_t download_rows[DOWNLOADS_SHOWN];
static lv_obj_t *download_values[DOWNLOADS_SHOWN];
static int download_count;
static char picked_id[ABS_ID_MAX];

static void picked_play(void *user) {
	(void)user;
	play_downloaded(picked_id);
}

static void picked_resume(void *user) {
	(void)user;
	absdl_entry_t d;
	if (absdl_get(picked_id, &d)) {
		start_download(d.id, d.title, d.author);
	}
}

static void picked_stop(void *user) {
	(void)user;
	absdl_cancel(picked_id);
}

static void picked_delete(void *user) {
	(void)user;
	absdl_entry_t d;
	if (absdl_get(picked_id, &d)) {
		ask_delete(d.id, d.title);
	}
}

static void download_row_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= download_count) {
		return;
	}
	const absdl_entry_t *d = &download_rows[i];
	snprintf(picked_id, sizeof(picked_id), "%s", d->id);
	popover_item_t items[2];
	int n = 0;
	switch (d->state) {
	case ABSDL_DONE:
		items[n++] = (popover_item_t){.label = tr("abs_play"), .action = picked_play};
		items[n++] = (popover_item_t){.label = tr("abs_delete_download"), .action = picked_delete};
		break;
	case ABSDL_PARTIAL:
		items[n++] = (popover_item_t){.label = tr("abs_resume_download"), .action = picked_resume};
		items[n++] = (popover_item_t){.label = tr("abs_delete_download"), .action = picked_delete};
		break;
	case ABSDL_QUEUED:
	case ABSDL_RUNNING:
		items[n++] = (popover_item_t){.label = tr("abs_stop_download"), .action = picked_stop};
		break;
	default:
		return;
	}
	popover_show(lv_event_get_current_target(e), items, n);
}

// Only the values move while a download runs; the rows are rebuilt when the
// list itself changes.
static void downloads_refresh(void) {
	absdl_entry_t now[DOWNLOADS_SHOWN];
	int count = absdl_list(now, DOWNLOADS_SHOWN);
	bool same = count == download_count;
	for (int i = 0; same && i < count; i++) {
		same = strcmp(now[i].id, download_rows[i].id) == 0;
	}
	memcpy(download_rows, now, (size_t)count * sizeof(*now));
	download_count = count;

	if (!same) {
		lv_obj_clean(downloads_container);
		for (int i = 0; i < count; i++) {
			lv_obj_t *row = settingsrow_add(downloads_container, now[i].title, &download_values[i], download_row_cb,
											(void *)(intptr_t)i);
			settingsrow_name_lines(row, 2);
			lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
			lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
			lv_obj_add_style(download_values[i], &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(download_values[i], &font_ui_20, 0);
		}
		downloads_empty = lv_label_create(downloads_container);
		lv_obj_set_width(downloads_empty, lv_pct(100));
		lv_label_set_long_mode(downloads_empty, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(downloads_empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_add_style(downloads_empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(downloads_empty, &font_ui_24, 0);
		lv_label_set_text(downloads_empty, tr("abs_downloads_empty"));
		lv_obj_set_hidden(downloads_empty, count > 0);
	}
	for (int i = 0; i < count; i++) {
		char status[240];
		download_status(&now[i], status, sizeof(status));
		lv_label_set_text(download_values[i], status);
	}
}

static void downloads_open(void) {
	download_count = -1; // rebuild
	downloads_refresh();
	switch_screen(downloads_screen);
}

static void build_downloads(void) {
	downloads_screen = lv_obj_create(NULL);
	downloads_container = settingsrow_page(downloads_screen, g_cfg, "abs_downloads");
	switcher_attach_back_gesture(downloads_screen);
}

// ---------------------------------------------------------------------------
// the answers, and the covers
// ---------------------------------------------------------------------------

static void poll_cb(lv_timer_t *t) {
	(void)t;
	lv_obj_t *active = lv_screen_active();
	if (active != home_screen && active != books_screen && active != names_screen && active != detail_screen &&
		active != search_screen && active != downloads_screen) {
		return;
	}

	unsigned s = abscatalog_serial();
	if (s != seen_serial) {
		seen_serial = s;
		abs_result_t res;
		abscatalog_result(&res);
		switch (waiting) {
		case WAIT_LIBRARIES:
			libraries_arrived(&res);
			break;
		case WAIT_BOOKS:
			books_arrived(&res);
			break;
		case WAIT_NAMES:
			names_arrived(&res);
			break;
		case WAIT_DETAIL:
			detail_arrived(&res);
			break;
		case WAIT_NONE:
			break;
		}
		if (res.state != ABS_RESULT_LOADING) {
			waiting = WAIT_NONE;
		}
	} else if (waiting != WAIT_NONE) {
		// The loading count moves without the serial moving.
		abs_result_t res;
		abscatalog_result(&res);
		if (res.state == ABS_RESULT_LOADING && waiting == WAIT_BOOKS) {
			books_arrived(&res);
		} else if (res.state == ABS_RESULT_LOADING && waiting == WAIT_NAMES) {
			names_arrived(&res);
		}
	}

	unsigned d = absdl_serial();
	if (d != seen_downloads) {
		seen_downloads = d;
		if (active == detail_screen) {
			detail_actions_refresh();
		} else if (active == books_screen) {
			books_rebind();
		} else if (active == downloads_screen) {
			downloads_refresh();
		}
	}

	if (active == books_screen) {
		books_art_update();
	} else if (active == detail_screen) {
		detail_art_update();
	}
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void abspage_open(void) { switch_screen(home_screen); }

void abspage_init(gui_config_t *cfg) {
	g_cfg = cfg;
	abscatalog_set_root(cfg->sd_root_path);
	absdl_set_root(cfg->sd_root_path);
	build_home();
	build_books();
	build_names();
	build_detail();
	build_search();
	build_downloads();
	seen_serial = abscatalog_serial();
	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
}
