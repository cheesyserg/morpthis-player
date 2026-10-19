/*
 * The Wi-Fi page's answer to a network that is not in its list -- the
 * "Add hidden network" row and the manual-entry page behind it -- on the
 * real renderer, at both panel sizes.
 *
 * The scan and the joining are checked elsewhere (test_wifi.c drives the
 * real worker against a fake wpa_cli); what is checked here is the part
 * that only exists with a screen: that the row is on the page and stays
 * inside the panel, that the page it opens holds its two fields and its
 * keyboard on the glass, that an empty name is refused where it stands,
 * and that what is typed reaches wifi_connect_hidden() -- the hidden join,
 * not the ordinary one -- with the password cleared behind it.
 *
 * The radio is substituted with a recording stub: one visible network in
 * the list, no scan ever running, and every join written down.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"

/* ---- fonts, as in the other page tests ---------------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold, font_ui_72;
static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
		&font_ui_64_bold, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) {
	assert(font_callback_count < 16);
	font_callbacks[font_callback_count++] = cb;
}
int fonts_text_size(void) { return text_size; }
bool fonts_set_text_size(int size) {
	text_size = size;
	for (int i = 0; i < font_callback_count; i++) {
		font_callbacks[i]();
	}
	return true;
}
const char *fonts_choice(void) { return "default"; }
bool fonts_set_face(const char *id) { return strcmp(id, "default") == 0; }
int fonts_choices(const char **ids, const char **labels, int max) {
	assert(max > 0);
	ids[0] = "default";
	labels[0] = "Test font";
	return 1;
}

/* ---- the parts of the shell the page leans on, substituted -------------- */

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"the card is there in this test"); }
void gui_notify_popup(const char *text) {
	fprintf(stderr, "popup: %s\n", text ? text : "");
	assert(!"unexpected popup");
}

static char last_error_toast[128];
void toast_error(const char *text) { snprintf(last_error_toast, sizeof(last_error_toast), "%s", text ? text : ""); }
void toast_plain(const char *text) { (void)text; }
void toast_success(const char *text) { (void)text; }
void toast_glyph(const lv_image_dsc_t *glyph, const char *text) {
	(void)glyph;
	(void)text;
}
void topbar_refresh_radios(void) {}

void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	(void)obj;
	(void)opening;
}
bool player_sheet_drag_active(void) { return false; }

/* ---- the radio, substituted and recording ------------------------------- */

static int hidden_joins;
static int plain_joins;
static char joined_ssid[WIFI_SSID_MAX];
static char joined_psk[128];

void wifi_init(void) {}
bool wifi_available(void) { return true; }
bool wifi_get_enabled(void) { return true; }
void wifi_set_enabled(bool on) { (void)on; }
void wifi_set_enabled_transient(bool on) { (void)on; }

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	out->state = WIFI_STATE_ON;
}

bool wifi_interface_address(char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}
void wifi_set_status_poll_slow(bool slow) { (void)slow; }
void wifi_scan_start(void) {}
bool wifi_scan_running(void) { return false; }
uint32_t wifi_networks_serial(void) { return 1; }

int wifi_get_networks(wifi_network_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	snprintf(out->ssid, sizeof(out->ssid), "CoffeeShop");
	out->signal = -52;
	out->bars = 3;
	out->secured = true;
	out->id = -1;
	return 1;
}

static void record_join(const char *ssid, const char *psk, bool hidden) {
	if (hidden) {
		hidden_joins++;
	} else {
		plain_joins++;
	}
	snprintf(joined_ssid, sizeof(joined_ssid), "%s", ssid ? ssid : "");
	snprintf(joined_psk, sizeof(joined_psk), "%s", psk ? psk : "");
}

void wifi_connect(const char *ssid, const char *psk) { record_join(ssid, psk, false); }
void wifi_connect_hidden(const char *ssid, const char *psk) { record_join(ssid, psk, true); }
void wifi_disconnect(void) {}
void wifi_forget(const char *ssid) { (void)ssid; }
wifi_op_t wifi_take_op_result(char *ssid_out, int ssid_size) {
	if (ssid_out && ssid_size > 0) {
		ssid_out[0] = '\0';
	}
	return WIFI_OP_IDLE;
}
wifi_fail_t wifi_last_failure(void) { return WIFI_FAIL_NONE; }
wifi_opkind_t wifi_last_op_kind(void) { return WIFI_OPKIND_CONNECT; }
bool wifi_busy(void) { return false; }

/* ---- small helpers ------------------------------------------------------- */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *button_of_label(lv_obj_t *root, const char *text) {
	lv_obj_t *label = find_label(root, text);
	assert(label);
	lv_obj_t *button = lv_obj_get_parent(label);
	while (button && !lv_obj_check_type(button, &lv_button_class)) {
		button = lv_obj_get_parent(button);
	}
	assert(button);
	return button;
}

/* Every textarea on a page, in the order they were built: the manual page's
 * name field first, its passphrase second. */
static int find_textareas(lv_obj_t *obj, lv_obj_t **out, int max) {
	int count = 0;
	if (lv_obj_check_type(obj, &lv_textarea_class)) {
		if (count < max) {
			out[count] = obj;
		}
		count++;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		count += find_textareas(lv_obj_get_child(obj, i), out + count, max - count);
	}
	return count;
}

/* Nothing drawn may leave the panel sideways; downwards only inside a
 * container that scrolls that way (the Wi-Fi page's own list). */
static void check_within_panel_in(lv_obj_t *obj, gui_config_t *cfg, const char *what, bool scrolls) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	bool overflow_x = area.x1 < 0 || area.x2 >= (int32_t)cfg->screen_width;
	bool overflow_y = area.y1 < 0 || area.y2 >= (int32_t)cfg->screen_height;
	if (overflow_x || (overflow_y && !scrolls)) {
		fprintf(stderr, "%s: an object at %d,%d..%d,%d is outside the %dx%d panel\n", what, area.x1, area.y1, area.x2,
				area.y2, (int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	if (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) {
		scrolls = true;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel_in(lv_obj_get_child(obj, i), cfg, what, scrolls);
	}
}

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("MORPTHIS_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

/* ---- the strings, in every language -------------------------------------- */

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

/* Every language file has to carry the new page's strings: tr() falls back
 * to the key it is given, so a missing one shows the tag itself on screen. */
static void check_strings(const char *language_dir) {
	static const char *const KEYS[] = {
		"wifi_add_hidden_network", "wifi_hidden_network", "wifi_network_name", "wifi_hidden_network_note",
		"wifi_enter_network_name",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
			assert(strlen(text) < 160);
		}
		languages++;
	}
	closedir(dir);
	assert(languages == 11);
	assert(lang_set("English"));
}

/* ---- the pages ------------------------------------------------------------ */

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/morpthis-wifi-ui-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	check_strings(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	wifisettings_screen = lv_obj_create(NULL);
	wifisettings_init(&cfg);

	/* The Wi-Fi page: the network in the air is listed, and under it the row
	 * for the one that never will be. */
	lv_screen_load(wifisettings_screen);
	/* One round of the page's poll, as the player's main loop would give it:
	 * it is what takes the list from "searching" to the networks the stub
	 * radio says it found. */
	lv_timer_handler();
	lv_obj_update_layout(wifisettings_screen);
	check_within_panel_in(wifisettings_screen, &cfg, "Wi-Fi", false);
	dump_snapshot(wifisettings_screen, bp_is_tempotec_v1() ? "v1-wifi" : "hiby-wifi");
	assert(find_label(wifisettings_screen, "CoffeeShop"));
	lv_obj_t *add_row = button_of_label(wifisettings_screen, tr("wifi_add_hidden_network"));
	assert(!lv_obj_is_hidden(add_row));
	lv_area_t row_area;
	lv_obj_get_coords(add_row, &row_area);
	assert(lv_obj_get_width(add_row) >= (int32_t)cfg.screen_width - 3 * (int32_t)cfg.padding);
	assert(lv_obj_get_height(add_row) >= bp_pick(44, 56));

	/* The manual-entry page it opens: two fields and the keyboard, all of
	 * them on the glass, starting empty and hidden-eyed. */
	lv_obj_send_event(add_row, LV_EVENT_CLICKED, NULL);
	lv_obj_t *manual = lv_screen_active();
	assert(manual != wifisettings_screen);
	lv_obj_update_layout(manual);
	check_within_panel_in(manual, &cfg, "Add hidden network", false);
	dump_snapshot(manual, bp_is_tempotec_v1() ? "v1-wifi-hidden" : "hiby-wifi-hidden");
	/* The page's heading is there, cut short by its own long-dot mode if it
	 * has to be: what matters here is the two fields and their prompts. */

	lv_obj_t *fields[2];
	assert(find_textareas(manual, fields, 2) == 2);
	lv_obj_t *ssid_field = fields[0];
	lv_obj_t *psk_field = fields[1];
	assert(strcmp(lv_textarea_get_text(ssid_field), "") == 0);
	assert(strcmp(lv_textarea_get_placeholder_text(ssid_field), tr("wifi_network_name")) == 0);
	assert(strcmp(lv_textarea_get_placeholder_text(psk_field), tr("password")) == 0);
	assert(lv_textarea_get_password_mode(psk_field));

	/* The keyboard's accent key with no name typed: refused where it
	 * stands, with the reason named, and nothing joined. */
	lv_obj_t *ok = button_of_label(manual, tr("ok"));
	lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL);
	assert(strcmp(last_error_toast, "wifi_enter_network_name") == 0);
	assert(hidden_joins == 0 && plain_joins == 0);
	assert(lv_screen_active() == manual);

	/* A name and a passphrase. The passphrase field is typed into through
	 * the keyboard itself -- one letter by key, to prove the keys were
	 * pointed at it by touching it -- and the rest set as a page that had
	 * been typed on would hold it. */
	lv_textarea_set_text(ssid_field, "HiddenNet");
	lv_obj_send_event(psk_field, LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(button_of_label(manual, "p"), LV_EVENT_CLICKED, NULL);
	assert(strcmp(lv_textarea_get_text(psk_field), "p") == 0);
	lv_textarea_set_text(psk_field, "correct horse");

	lv_obj_send_event(ok, LV_EVENT_CLICKED, NULL);
	assert(hidden_joins == 1 && plain_joins == 0);
	assert(strcmp(joined_ssid, "HiddenNet") == 0);
	assert(strcmp(joined_psk, "correct horse") == 0);
	/* Back on the Wi-Fi page, with the passphrase gone from its field. */
	assert(lv_screen_active() == wifisettings_screen);
	assert(strcmp(lv_textarea_get_text(psk_field), "") == 0);

	/* And the page opens empty again, name included: what was joined a
	 * moment ago is not offered for joining again. */
	lv_obj_send_event(add_row, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == manual);
	assert(strcmp(lv_textarea_get_text(ssid_field), "") == 0);
	assert(strcmp(lv_textarea_get_text(psk_field), "") == 0);

	lv_deinit();
	printf("PASS wifi pages (%s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720");
	return 0;
}
