#include "abssettings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/abs/abs.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

#define TICK_MS 500

// An API key is a few hundred characters: nobody types that on this keyboard.
// It is read from this file at the root of the card instead, and the file is
// deleted once the key has worked -- a card goes into other computers.
#define KEY_FILE "audiobookshelf.txt"

static lv_obj_t *page_screen;
static lv_obj_t *account_value;
static lv_obj_t *server_value;
static lv_obj_t *status_value;

static lv_obj_t *login_screen;
static lv_obj_t *server_field;
static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static const char *card_root;

static unsigned seen_login_serial;
static bool waiting_for_login;
static bool login_from_key_file;

lv_obj_t *abssettings_screen(void) { return page_screen; }

static bool online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0];
}

// The server address without its scheme: the row has little room, and the
// scheme is not what anyone looks for.
static const char *shown_server(const char *server) {
	const char *p = strstr(server, "://");
	return p ? p + 3 : server;
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void refresh(void) {
	abs_status_t st;
	abs_get_status(&st);

	lv_label_set_text(account_value, st.user[0] ? st.user : tr("abs_not_signed_in"));
	lv_label_set_text(server_value, st.server[0] ? shown_server(st.server) : "-");

	const char *status = "";
	switch (st.state) {
	case ABS_STATE_SIGNED_OUT:
		status = tr("abs_not_signed_in");
		break;
	case ABS_STATE_SIGNING_IN:
		status = tr("abs_signing_in");
		break;
	case ABS_STATE_SIGNED_IN:
		status = tr("signed_in");
		break;
	case ABS_STATE_EXPIRED:
		status = tr("abs_expired");
		break;
	}
	lv_label_set_text(status_value, status);
}

static void focus_field(lv_obj_t *field) {
	lv_obj_t *fields[] = {server_field, user_field, password_field};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		bool on = fields[i] == field;
		if (on) {
			lv_obj_add_state(fields[i], LV_STATE_FOCUSED);
		} else {
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
		}
		keyboard_show_caret(fields[i], on);
	}
	keyboard_set_field(login_keyboard, field);
}

static void open_login(void) {
	abs_status_t st;
	abs_get_status(&st);
	// Signing in again after the session ran out is the usual case: the same
	// server and the same user, only the password to type.
	const char *last_server = st.server[0] ? st.server : config_get("audiobookshelf", "last_server", "");
	lv_textarea_set_text(server_field, last_server);
	lv_textarea_set_text(user_field, st.user);
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	lv_image_set_src(password_eye_icon, &icon_eye);
	keyboard_refresh_password(password_field);
	keyboard_reset(login_keyboard);
	focus_field(!last_server[0] ? server_field : !st.user[0] ? user_field : password_field);
	switch_screen(login_screen);
}

static void do_logout(void *user) {
	(void)user;
	abs_logout();
	toast_success("signed_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	abs_status_t st;
	abs_get_status(&st);
	if (st.state == ABS_STATE_SIGNING_IN) {
		return;
	}
	if (st.state == ABS_STATE_SIGNED_IN) {
		char message[160];
		snprintf(message, sizeof(message), tr("sign_out_confirm_note"), "Audiobookshelf");
		confirm_show("abs_sign_out", message, "leave", do_logout, NULL);
		return;
	}
	// Signed out, or expired: the way back in.
	open_login();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page_screen, cfg, "audiobookshelf");

	settingsrow_add(container, "abs_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "abs_server", &server_value, NULL, NULL);
	settingsrow_add(container, "abs_status", &status_value, NULL, NULL);

	// A server address or a long user name: one line, cut with an ellipsis.
	lv_obj_t *values[] = {account_value, server_value, status_value};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(values[i], lv_pct(60), 0);
		lv_obj_set_height(values[i], lv_font_get_line_height(&font_ui_24));
	}

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("abs_note"));

	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

// ---------------------------------------------------------------------------
// signing in
// ---------------------------------------------------------------------------

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = !lv_textarea_get_password_mode(password_field);
	lv_textarea_set_password_mode(password_field, hidden);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(password_field);
}

// The keyboard writes into the last field touched.
static void field_focus_cb(lv_event_t *e) { focus_field(lv_event_get_target(e)); }

static bool start_checks(void) {
	if (!online()) {
		toast_error("enable_wi_fi_first");
		return false;
	}
	return true;
}

static void begin_waiting(const char *server) {
	// Offered again next time, also after signing out.
	config_set("audiobookshelf", "last_server", server);
	waiting_for_login = true;
	toast_busy("abs_signing_in");
}

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	char server[256];
	if (!abs_normalise_server(lv_textarea_get_text(server_field), server, sizeof(server))) {
		toast_error("abs_server_required");
		focus_field(server_field);
		return;
	}
	const char *name = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!name || !name[0] || !password || !password[0]) {
		toast_error("credentials_required");
		return;
	}
	if (!start_checks()) {
		return;
	}
	abs_login_password(server, name, password);
	lv_textarea_set_text(password_field, "");
	login_from_key_file = false;
	begin_waiting(server);
}

// "server = ..." and "api_key = ..." out of the file on the card. Blank lines,
// comments and spaces around '=' are allowed: it is written by hand.
static bool read_key_file(char *path, size_t path_size, char *server, size_t server_size, char *key, size_t key_size) {
	if (!card_root || !card_root[0]) {
		return false;
	}
	snprintf(path, path_size, "%s/%s", card_root, KEY_FILE);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	server[0] = key[0] = '\0';
	char line[2048];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';') {
			continue;
		}
		*eq = '\0';
		char *k = line, *v = eq + 1;
		while (isspace((unsigned char)*k)) {
			k++;
		}
		for (char *end = eq - 1; end >= k && isspace((unsigned char)*end); end--) {
			*end = '\0';
		}
		while (isspace((unsigned char)*v)) {
			v++;
		}
		for (char *end = v + strlen(v) - 1; end >= v && isspace((unsigned char)*end); end--) {
			*end = '\0';
		}
		if (strcasecmp(k, "server") == 0) {
			snprintf(server, server_size, "%s", v);
		} else if (strcasecmp(k, "api_key") == 0) {
			snprintf(key, key_size, "%s", v);
		}
	}
	memset(line, 0, sizeof(line));
	fclose(f);
	return key[0] != '\0';
}

static void key_file_cb(lv_event_t *e) {
	(void)e;
	char path[600], typed_server[256] = "", file_server[256], key[2048];
	if (!read_key_file(path, sizeof(path), file_server, sizeof(file_server), key, sizeof(key))) {
		toast_error("abs_key_file_missing");
		return;
	}
	// The address in the file wins; the one typed above is the fallback.
	const char *server_text = file_server[0] ? file_server : lv_textarea_get_text(server_field);
	if (!abs_normalise_server(server_text, typed_server, sizeof(typed_server))) {
		memset(key, 0, sizeof(key));
		toast_error("abs_server_required");
		return;
	}
	if (!start_checks()) {
		memset(key, 0, sizeof(key));
		return;
	}
	abs_login_api_key(typed_server, key);
	memset(key, 0, sizeof(key));
	login_from_key_file = true;
	begin_waiting(typed_server);
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "abs_sign_in");
	int top = settingsrow_content_top(cfg);

	server_field = make_field(login_screen, cfg, "abs_server_placeholder", top);
	user_field = make_field(login_screen, cfg, "abs_username", top + 78);
	password_field = make_field(login_screen, cfg, "password", top + 156);
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 156 + 3);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_image_set_src(password_eye_icon, &icon_eye);
	lv_obj_center(password_eye_icon);

	// The other way in, as a link: an API key from the card.
	lv_obj_t *key_link = lv_label_create(login_screen);
	lv_label_set_long_mode(key_link, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(key_link, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(key_link, LV_ALIGN_TOP_LEFT, cfg->padding, top + 230);
	lv_obj_set_style_text_color(key_link, theme()->accent, 0);
	lv_obj_set_style_text_font(key_link, &font_ui_18, 0);
	lv_label_set_text(key_link, tr("abs_use_key_file"));
	lv_obj_set_clickable(key_link, true);
	lv_obj_set_ext_click_area(key_link, 10);
	lv_obj_add_event_cb(key_link, key_file_cb, LV_EVENT_CLICKED, NULL);

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, server_field, NULL, "ok", login_accept_cb, NULL);
	switcher_attach_back_gesture(login_screen);
}

static void login_finished(const abs_status_t *st) {
	if (!waiting_for_login) {
		return;
	}
	waiting_for_login = false;
	toast_busy_end();

	char message[200];
	switch (st->login_result) {
	case ABS_LOGIN_OK:
		if (login_from_key_file) {
			char path[600], server[256], key[2048];
			if (read_key_file(path, sizeof(path), server, sizeof(server), key, sizeof(key))) {
				unlink(path);
			}
			memset(key, 0, sizeof(key));
		}
		if (lv_screen_active() == login_screen) {
			back_btn_cb(NULL);
		}
		toast_success("signed_in");
		break;
	case ABS_LOGIN_WRONG_CREDENTIALS:
		toast_error(login_from_key_file ? "abs_wrong_key" : "abs_wrong_credentials");
		break;
	case ABS_LOGIN_NO_REPLY:
		toast_error(st->detail[0] ? st->detail : "abs_no_reply");
		break;
	case ABS_LOGIN_NOT_ABS:
		toast_error("abs_not_abs");
		break;
	case ABS_LOGIN_TOO_OLD:
		snprintf(message, sizeof(message), tr("abs_too_old"), st->detail, ABS_MIN_VERSION);
		toast_error(message);
		break;
	case ABS_LOGIN_NO_PASSWORD:
		toast_error("abs_no_password");
		break;
	case ABS_LOGIN_REFUSED:
		toast_error(st->detail);
		break;
	}
	login_from_key_file = false;
}

static void tick_cb(lv_timer_t *t) {
	(void)t;
	abs_status_t st;
	abs_get_status(&st);
	if (st.login_serial != seen_login_serial) {
		seen_login_serial = st.login_serial;
		login_finished(&st);
	}
	if (lv_screen_active() == page_screen) {
		refresh();
	}
}

void abssettings_init(gui_config_t *cfg) {
	card_root = cfg->sd_root_path;
	build_page(cfg);
	build_login(cfg);

	abs_status_t st;
	abs_get_status(&st);
	seen_login_serial = st.login_serial;
	refresh();

	lv_timer_create(tick_cb, TICK_MS, NULL);
}
