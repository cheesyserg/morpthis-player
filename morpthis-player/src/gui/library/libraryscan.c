#include "libraryscan.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/toast.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/playlist.h"

#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/device/power.h"

// Fast enough that the number never looks stuck, slow enough that redrawing it
// costs nothing next to reading tags off a card.
#define SCAN_POLL_MS 200
#define SCAN_BUTTON_WIDTH bp_pick(184, 240)
#define SCAN_BUTTON_HEIGHT bp_pick(52, 68)
#define SCAN_BUTTON_BOTTOM bp_pick(12, 30)
#define SCAN_CONTENT_GAP bp_pick(10, 16)
// The space between the four things in the middle of the page: the note and
// the three lines of text under it.
#define SCAN_ROW_GAP bp_pick(6, 10)
// The note is sized from what those lines leave (see scan_fit_note() below);
// these are only the bounds. Its source art is 128 px, which is what the
// 480 px players draw it at.
#define SCAN_NOTE_MAX bp_pick(96, 128)
#define SCAN_NOTE_MIN 32

lv_obj_t *libraryscan_screen;

static lv_obj_t *count_label;
static lv_obj_t *status_label;
static lv_obj_t *progress_label;
static lv_obj_t *ok_button;
static lv_obj_t *cancel_button;
static lv_obj_t *scan_note;
static lv_obj_t *scan_container;
static lv_timer_t *poll_timer;
static char shown_file[512];

static const char *sd_root;

static void show_finished(int found) {
	lv_label_set_text_fmt(count_label, "%d", found);
	lv_label_set_text(status_label, found == 1 ? tr("libraryscan_track_found") : tr("libraryscan_tracks_found"));
	lv_obj_set_hidden(progress_label, true);

	lv_obj_set_hidden(cancel_button, true);
	lv_obj_set_hidden(ok_button, false);

	// The scan is over; the screen may go back to timing out.
	power_hold_screen_on(false);
}

static void poll_cb(lv_timer_t *timer) {
	int found = library_scan_found();

	if (!library_scan_running()) {
		lv_timer_pause(timer);
		show_finished(found);
		return;
	}

	// The artwork pass has the page to itself while it runs. The track count is
	// final by then, and a number that stops moving for minutes reads as a
	// stuck scan -- so the pass drives the same counter the scan itself used,
	// with its own total under it.
	int art_done = 0, art_total = 0;
	if (library_scan_artwork_state(&art_done, &art_total)) {
		lv_label_set_text_fmt(count_label, "%d", art_done);
		lv_label_set_text_fmt(status_label, tr("libraryscan_artwork_of"), art_total);

		char line[64];
		snprintf(line, sizeof(line), "%d%%", art_total ? art_done * 100 / art_total : 0);
		lv_label_set_text(progress_label, line);
		lv_obj_set_hidden(progress_label, false);
		return;
	}

	lv_label_set_text_fmt(count_label, "%d", found);
	lv_label_set_text(status_label, found == 1 ? tr("libraryscan_track_found") : tr("libraryscan_tracks_found"));

	char path[512];
	library_scan_current_file(path, sizeof(path));
	if (!path[0]) {
		// Before the first file is opened, or while a directory is being
		// walked, show where the scan is instead of leaving a blank line.
		library_scan_current_folder(path, sizeof(path));
	}
	const char *item = strrchr(path, '/');
	item = item ? item + 1 : path;
	if (item[0]) {
		if (strcmp(shown_file, item) != 0) {
			snprintf(shown_file, sizeof(shown_file), "%s", item);
			lv_label_set_text(progress_label, shown_file);
		}
		lv_obj_set_hidden(progress_label, false);
	} else {
		lv_obj_set_hidden(progress_label, true);
	}
}

// Back to the Music page, which now shows the library, with the menu as the
// only page behind it: walking back through a finished scan makes no sense.
static void back_to_music(void) {
	screen_history_reset();
	switch_screen_no_history(music_screen);
}

static void ok_cb(lv_event_t *e) {
	(void)e;
	back_to_music();
}

// Stopping is not throwing away: the scan thread winds up at the next file and
// commits everything it has already read, so the index holds exactly the
// tracks the counter was showing.
static void cancel_cb(lv_event_t *e) {
	(void)e;

	library_scan_stop();
	lv_timer_pause(poll_timer);
	power_hold_screen_on(false);

	back_to_music();
}

void libraryscan_begin(void) {
	// A pass from a previous run must not show through this one's counter.
	library_scan_artwork_done();

	lv_label_set_text(count_label, "0");
	lv_label_set_text(status_label, tr("libraryscan_tracks_found"));
	shown_file[0] = '\0';
	lv_label_set_text(progress_label, "");
	lv_obj_set_hidden(progress_label, true);
	lv_obj_set_hidden(ok_button, true);
	lv_obj_set_hidden(cancel_button, false);

	if (!library_scan_start(sd_root)) {
		// Nothing to scan (no card, or a scan is somehow already going).
		lv_label_set_text(status_label, tr("no_card_to_scan"));
		lv_obj_set_hidden(cancel_button, true);
		lv_obj_set_hidden(ok_button, false);
		return;
	}

	// Reading a card takes minutes and nobody is touching the screen while it
	// happens; the idle timer must not blank the panel halfway through.
	power_hold_screen_on(true);

	lv_timer_reset(poll_timer);
	lv_timer_resume(poll_timer);
}

// ---------------------------------------------------------------------------
// Which folders: the card's top-level folders, ticked, before the scan starts
// ---------------------------------------------------------------------------

#define PICK_CHROME_H bp_pick(180, 250) // the card's title, note, buttons and padding
#define PICK_CARD_PAD bp_pick(10, 20)
#define PICK_CARD_GAP bp_pick(6, 12)
#define PICK_LIST_GAP bp_pick(4, 6)
#define PICK_ROW_HEIGHT bp_pick(44, 60)
#define PICK_ROW_PAD bp_pick(10, 16)
#define PICK_ROW_GAP bp_pick(6, 10)
#define PICK_BUTTON_HEIGHT bp_pick(44, 56)
#define PICK_BUTTON_GAP bp_pick(8, 12)

// What a screen of the picker can show. The folders are a list of names that
// can run to hundreds, so only these rows exist: the scroller moves the body
// they sit on and each row is bound to whatever folder is under it now.
#define PICK_ROW_POOL 14
#define PICK_ROW_PITCH (PICK_ROW_HEIGHT + PICK_LIST_GAP)

typedef struct {
	lv_obj_t *button;
	lv_obj_t *label;
	lv_obj_t *mark;
	int index;
} pick_row_t;

static gui_config_t *pick_cfg;
static lv_obj_t *pick_veil;
static lv_obj_t *pick_list;
static lv_obj_t *pick_body;
static lv_obj_t *pick_scan_btn;
static char **pick_names;
static bool *pick_on;
static pick_row_t pick_rows[PICK_ROW_POOL];
static int pick_pool_first = -1;
static int pick_count;
static int pick_capacity;

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

static void pick_clear(void) {
	for (int i = 0; i < pick_count; i++) {
		free(pick_names[i]);
	}
	pick_count = 0;
}

static bool pick_add(const char *name) {
	if (pick_count == pick_capacity) {
		int grown = pick_capacity ? pick_capacity * 2 : 64;
		char **names = realloc(pick_names, (size_t)grown * sizeof(*names));
		if (names) {
			pick_names = names;
		}
		bool *on = realloc(pick_on, (size_t)grown * sizeof(*on));
		if (on) {
			pick_on = on;
		}
		if (!names || !on) {
			return false;
		}
		pick_capacity = grown;
	}
	pick_names[pick_count] = strdup(name);
	if (!pick_names[pick_count]) {
		return false;
	}
	pick_on[pick_count] = false;
	pick_count++;
	return true;
}

// The folders at the root of the card, alphabetically: not the hidden ones,
// not the ones a desktop leaves behind, and not Audiobooks or the downloads
// folder older builds left behind, which the music scan never reads.
static void pick_read_folders(void) {
	pick_clear();
	DIR *dir = sd_root ? opendir(sd_root) : NULL;
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name) || strcasecmp(de->d_name, AUDIOBOOKDB_FOLDER) == 0 || strcasecmp(de->d_name, LIBRARY_LEGACY_DOWNLOADS_FOLDER) == 0) {
			continue;
		}
		char path[768];
		if (snprintf(path, sizeof(path), "%s/%s", sd_root, de->d_name) >= (int)sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}
		if (!pick_add(de->d_name)) {
			break;
		}
	}
	closedir(dir);
	if (pick_count > 1) {
		qsort(pick_names, (size_t)pick_count, sizeof(pick_names[0]), name_cmp);
	}
}

// Ticked: the folders saved last time, or every folder when nothing was saved.
static void pick_load_selection(void) {
	int saved_count = 0;
	char **saved = library_scan_folders(&saved_count);
	for (int i = 0; i < pick_count; i++) {
		pick_on[i] = saved_count == 0;
		for (int j = 0; j < saved_count && !pick_on[i]; j++) {
			pick_on[i] = strcasecmp(pick_names[i], saved[j]) == 0;
		}
	}
	library_scan_folders_free(saved, saved_count);
}

static void pick_row_paint(pick_row_t *row) {
	if (!row->mark || row->index < 0 || row->index >= pick_count) {
		return;
	}
	lv_obj_set_style_image_opa(row->mark, pick_on[row->index] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void pick_paint(void) {
	int on = 0;
	for (int i = 0; i < pick_count; i++) {
		on += pick_on[i];
	}
	for (int i = 0; i < PICK_ROW_POOL; i++) {
		pick_row_paint(&pick_rows[i]);
	}
	lv_obj_set_style_opa(pick_scan_btn, (pick_count == 0 || on > 0) ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void pick_row_cb(lv_event_t *e) {
	pick_row_t *row = lv_event_get_user_data(e);
	if (row && row->index >= 0 && row->index < pick_count) {
		pick_on[row->index] = !pick_on[row->index];
		pick_paint();
	}
}

static void pick_close(void) { lv_obj_set_hidden(pick_veil, true); }

static void pick_veil_cb(lv_event_t *e) {
	if (lv_event_get_target(e) == pick_veil) {
		pick_close();
	}
}

static void pick_cancel_cb(lv_event_t *e) {
	(void)e;
	pick_close();
}

// Every folder ticked is the whole card, the files at its root included.
static void pick_scan_cb(lv_event_t *e) {
	(void)e;
	const char **chosen = malloc((size_t)(pick_count ? pick_count : 1) * sizeof(*chosen));
	if (!chosen) {
		return;
	}
	int count = 0;
	for (int i = 0; i < pick_count; i++) {
		if (pick_on[i]) {
			chosen[count++] = pick_names[i];
		}
	}
	if (pick_count > 0 && count == 0) {
		free(chosen);
		toast_error(tr("libraryscan_choose_a_folder"));
		return;
	}
	library_scan_folders_set(chosen, count == pick_count ? 0 : count);
	free(chosen);
	pick_close();
	switch_screen(libraryscan_screen);
}

static lv_obj_t *pick_button(lv_obj_t *parent, const char *text, bool accent, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, PICK_BUTTON_HEIGHT);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, accent ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_text_color(label, accent ? theme_on_accent() : theme()->text_primary, 0);
	lv_obj_center(label);
	return btn;
}

static void pick_scroll_cb(lv_event_t *e);

static void pick_build(void) {
	gui_config_t *cfg = pick_cfg;
	pick_veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pick_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(pick_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pick_veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(pick_veil, 0, 0);
	lv_obj_set_style_radius(pick_veil, 0, 0);
	lv_obj_set_style_pad_all(pick_veil, 0, 0);
	lv_obj_set_scrollable(pick_veil, false);
	lv_obj_set_clickable(pick_veil, true);
	lv_obj_set_hidden(pick_veil, true);
	lv_obj_add_event_cb(pick_veil, pick_veil_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(pick_veil);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding, 0);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 16, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, PICK_CARD_PAD, 0);
	lv_obj_set_style_pad_row(card, PICK_CARD_GAP, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_set_event_bubble(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_align(card, LV_ALIGN_CENTER, 0, cfg->top_bar_height / 2);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("musicsettings_scan_the_music_library"));
	lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, bp_is_tempotec_v1() ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *note = lv_label_create(card);
	lv_label_set_text(note, tr("libraryscan_choose_folders_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, bp_is_tempotec_v1() ? &font_ui_14 : &font_ui_18, 0);

	// The folders scroll; the title and the buttons stay put.
	pick_list = lv_obj_create(card);
	lv_obj_remove_style_all(pick_list);
	lv_obj_set_width(pick_list, lv_pct(100));
	lv_obj_set_height(pick_list, LV_SIZE_CONTENT);
	// What the panel leaves once the title, the note and the buttons are in.
	lv_obj_set_style_max_height(pick_list, cfg->screen_height - cfg->top_bar_height - 2 * cfg->padding - PICK_CHROME_H,
								0);
	lv_obj_set_scroll_dir(pick_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(pick_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(pick_list, pick_scroll_cb, LV_EVENT_SCROLL, NULL);

	// All the folders as one canvas under a fixed pool of rows. A card with two
	// hundred folders then scrolls the canvas, not two hundred buttons, and only
	// the ones on screen are bound to a name.
	pick_body = lv_obj_create(pick_list);
	lv_obj_remove_style_all(pick_body);
	lv_obj_set_width(pick_body, lv_pct(100));
	lv_obj_set_height(pick_body, 0);
	lv_obj_set_scrollable(pick_body, false);
	lv_obj_set_clickable(pick_body, false);

	for (int i = 0; i < PICK_ROW_POOL; i++) {
		pick_row_t *row = &pick_rows[i];
		row->index = -2;

		row->button = lv_btn_create(pick_body);
		lv_obj_set_size(row->button, lv_pct(100), PICK_ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_set_style_bg_color(row->button, theme()->surface_pressed, 0);
		lv_obj_set_style_radius(row->button, 10, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, PICK_ROW_PAD, 0);
		lv_obj_set_style_pad_column(row->button, PICK_ROW_GAP, 0);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		// A press on a partly visible folder starts a scroll, not a request to
		// animate that folder into view.
		lv_obj_set_scroll_on_focus(row->button, false);
		lv_obj_set_hidden(row->button, true);
		lv_obj_add_event_cb(row->button, pick_row_cb, LV_EVENT_CLICKED, row);

		lv_obj_t *folder = lv_image_create(row->button);
		lv_image_set_src(folder, &icon_folder);
		lv_obj_add_style(folder, &theme_style_icon, 0);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		const lv_font_t *row_font = bp_is_tempotec_v1() ? &font_ui_16 : &font_ui_22;
		lv_obj_set_height(row->label, lv_font_get_line_height(row_font));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, row_font, 0);

		row->mark = lv_image_create(row->button);
		lv_image_set_src(row->mark, &icon_check);
		lv_obj_add_style(row->mark, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor(row->mark, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(row->mark, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(row->mark, LV_OPA_TRANSP, 0);
	}

	lv_obj_t *buttons = lv_obj_create(card);
	lv_obj_remove_style_all(buttons);
	lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(buttons, PICK_BUTTON_GAP, 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	pick_button(buttons, "cancel", false, pick_cancel_cb);
	pick_scan_btn = pick_button(buttons, "scan", true, pick_scan_cb);
}

// Points one pool row at folder `index`, reading its name in place.
static void pick_row_bind(pick_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	if (index < 0 || index >= pick_count) {
		row->index = -1;
		lv_obj_set_hidden(row->button, true);
		return;
	}

	row->index = index;
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * PICK_ROW_PITCH);
	lv_label_set_text(row->label, pick_names[index]);
	pick_row_paint(row);
}

static int pick_pool_first_for(void) {
	if (pick_count <= 0) {
		return 0;
	}
	int scroll = lv_obj_get_scroll_y(pick_list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = scroll / PICK_ROW_PITCH - 1;
	if (first + PICK_ROW_POOL > pick_count) {
		first = pick_count - PICK_ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	return first;
}

static void pick_window_update(void) {
	int first = pick_pool_first_for();
	for (int i = 0; i < PICK_ROW_POOL; i++) {
		int index = first + i;
		pick_row_bind(&pick_rows[index % PICK_ROW_POOL], index < pick_count ? index : -1);
	}
	pick_pool_first = first;
}

static void pick_scroll_cb(lv_event_t *e) {
	(void)e;
	if (pick_pool_first_for() != pick_pool_first) {
		pick_window_update();
	}
}

static void pick_fill(void) {
	for (int i = 0; i < PICK_ROW_POOL; i++) {
		pick_rows[i].index = -2; // the folder under this row is a different one now
		lv_obj_set_hidden(pick_rows[i].button, true);
	}
	pick_pool_first = -1;
	lv_obj_set_height(pick_body, pick_count > 0 ? pick_count * PICK_ROW_PITCH - PICK_LIST_GAP : 0);
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	pick_window_update();
}

void libraryscan_choose_folders(void) {
	if (!pick_veil) {
		pick_build();
	}
	pick_read_folders();
	pick_load_selection();
	pick_fill();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	pick_paint();
	lv_obj_set_hidden(pick_veil, false);
	lv_obj_move_foreground(pick_veil);
}

// Both buttons sit at the same place at the bottom of the screen; only one is
// ever visible.
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t colour, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, SCAN_BUTTON_WIDTH, SCAN_BUTTON_HEIGHT);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -SCAN_BUTTON_BOTTOM);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, theme_ink_on(colour), 0);
	lv_obj_center(label);

	return button;
}

// ---------------------------------------------------------------------------
// Fitting the note
//
// The note is the one thing on this page that can give way: the count, the
// status line and the name of the file being read are text, and a cut-off line
// at the bottom is exactly what this page is there to report.
//
// Its source art is 128 px, nearly the whole of the gap between the page title
// and the button on the V1 -- more than the three lines of text put together.
// A flex column centres its overflow, so the note used to run up under the
// title while the file name was clipped at the other end: clipped and not
// pushed off, because this container does not scroll.
//
// So the note takes what the text leaves. Both its draw scale and its layout
// box are set: lv_image_set_scale() alone would leave the image's intrinsic
// 128 px box in the column and push the text out just the same (gridpage.c
// has the same story for the menu art). On the V1 the fit lands at about a
// quarter of the panel width, the proportion the art has on the 480 px
// players, and on those the result is the 128 px it was always drawn at.
// ---------------------------------------------------------------------------

static void scan_fit_note(void) {
	if (!scan_container || !scan_note || !count_label || !status_label || !progress_label) {
		return;
	}

	// On the V1 the file name is one line ending in an ellipsis, the way
	// LV_LABEL_LONG_DOT is meant to draw it. A label left to size itself never
	// overflows its own height, so LVGL never brings the dots in and a long
	// name simply wraps to a second line -- which is the line this page was
	// clipping. Pinning the height gives the ellipsis a boundary, as the tile
	// captions do in gridpage.c.
	if (bp_is_tempotec_v1()) {
		lv_obj_set_height(progress_label, lv_font_get_line_height(&font_ui_16));
		lv_obj_set_style_text_line_space(progress_label, 0, 0);
	}

	// The file name is hidden between files and a hidden object takes no part
	// in the layout, so it is measured with the flag off: the page has to be
	// laid out for the moment the name is there. The status sentence is
	// measured too, because in some languages it wraps to two lines.
	bool hidden = lv_obj_is_hidden(progress_label);
	lv_obj_set_hidden(progress_label, false);
	lv_obj_update_layout(scan_container);
	int text = lv_obj_get_height(count_label) + lv_obj_get_height(status_label) +
			   lv_obj_get_height(progress_label);
	if (hidden) {
		lv_obj_set_hidden(progress_label, true);
	}

	// Note, count, status line, file name: three gaps between the four, and
	// the note's own pad_bottom under it. `size` is the side of the note as it
	// is drawn; the box it takes in the column is that plus the padding, which
	// is what lv_obj_set_size() sizes.
	int pad_bottom = lv_obj_get_style_pad_bottom(scan_note, 0);
	int size = lv_obj_get_height(scan_container) - lv_obj_get_style_pad_top(scan_container, 0) -
			   lv_obj_get_style_pad_bottom(scan_container, 0) - text - SCAN_ROW_GAP * 3 - pad_bottom;

	if (size > SCAN_NOTE_MAX) {
		size = SCAN_NOTE_MAX;
	}
	if (size < SCAN_NOTE_MIN) {
		size = SCAN_NOTE_MIN;
	}

	lv_obj_set_size(scan_note, size, size + pad_bottom);
	lv_image_set_inner_align(scan_note, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(scan_note, (uint32_t)((int64_t)LV_SCALE_NONE * size / (int)icon_music_note.header.h));
}

// ---------------------------------------------------------------------------
// What the user is told about the runs on the scan thread
//
// Detect changes works in the background: a glyph in the status bar while it
// looks over the card, a check mark for a few seconds when it is done, and no
// notice either way. Filing the index again after the scan options changed was
// asked for on that page, so it keeps its notice and its outcome; filed again
// on its own, at the end of a Detect changes run, it says nothing.
// ---------------------------------------------------------------------------

typedef struct {
	library_update_event_t event;
	int added;
	int removed;
	int updated;
} update_note_t;

static bool checking;	   // a Detect changes run is under way
static bool reorganizing;  // library_reorganize() put its notice up

static void update_note_cb(void *user) {
	update_note_t *note = user;

	switch (note->event) {
	case LIBRARY_UPDATE_LOOKING:
		checking = true;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKING);
		break;

	case LIBRARY_UPDATE_ADDING:
		break;

	case LIBRARY_UPDATE_FINISHED:
		checking = false;
		topbar_set_library_check(TOPBAR_LIBRARY_CHECKED);
		break;

	case LIBRARY_UPDATE_STOPPED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
		} else if (checking) {
			checking = false;
			topbar_set_library_check(TOPBAR_LIBRARY_IDLE);
		}
		break;

	case LIBRARY_UPDATE_REORGANIZING:
		reorganizing = true;
		toast_busy_dismissable("libraryscan_reorganizing");
		break;

	case LIBRARY_UPDATE_REORGANIZED:
		if (reorganizing) {
			reorganizing = false;
			toast_busy_end();
			toast_success("libraryscan_reorganized");
		}
		break;

	case LIBRARY_UPDATE_SCANNED:
		// The first scan there has ever been turns Detect changes on, unless it
		// was already set either way.
		if (!library_detect_changes_chosen()) {
			library_set_detect_changes(true);
		}
		break;
	}
	free(note);
}

// On the scan thread: everything it says goes over to the interface thread.
static void update_listener(library_update_event_t event, int added, int removed, int updated) {
	update_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->added = added;
	note->removed = removed;
	note->updated = updated;
	if (!gui_post(update_note_cb, note)) {
		free(note);
	}
}

// How long after startup the card is looked over: past the first screen and
// the track being restored, which want the card first.
#define BOOT_CHECK_DELAY_MS 5000

// What library_card_returned() does for a card that comes back: Detect changes
// when it is on, and the index filed again if it was filed under other
// settings. A card mounted only after startup goes through it when it is
// attached instead (storage), and finds the library closed here.
static void boot_check_cb(lv_timer_t *timer) {
	(void)timer;
	// A library scanned before Detect changes came on by itself gets it the
	// same way, unless it was set either way since.
	if (!library_detect_changes_chosen() && library_is_open() && library_track_count() > 0) {
		library_set_detect_changes(true);
	}
	library_card_returned(sd_root);
}

// Runs on the scan thread after a full scan has committed, with the progress
// page still up: turns the fresh index into thumbnails on the card, so the
// first look at Albums is a read instead of a dozen decodes. The cost lands
// here, once per album, rather than on the page every time it is opened.
//
// Stop still works: cover_precache_albums() polls library_scan_stopping()
// between albums, so at worst one cover is finished before it winds up.
static void precache_progress(int done, int total, void *user) {
	(void)user;
	// Straight into the page's counter, read on the interface thread: tr() is
	// not safe from this one, and a formatted string would be translated there
	// anyway.
	library_scan_artwork_progress(done, total);
}

static void precache_finish_hook(void) {
	cover_precache_albums(library_scan_stopping, precache_progress, NULL);
	library_scan_artwork_done();
}

void libraryscan_init(gui_config_t *cfg) {
	sd_root = cfg->sd_root_path;
	pick_cfg = cfg;
	library_set_update_listener(update_listener);
	library_set_scan_finish_hook(precache_finish_hook);

	lv_obj_add_style(libraryscan_screen, &theme_style_screen, 0);

	// Centred: this page has no back chevron beside it to align with.
	lv_obj_t *title = settingsrow_title(libraryscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);
	int content_height = cfg->screen_height - content_top;
	if (bp_is_tempotec_v1()) {
		// Keep the live count and the file being read above the bottom action,
		// which otherwise covers the last line on this short panel.
		content_height -= SCAN_BUTTON_HEIGHT + SCAN_BUTTON_BOTTOM + SCAN_CONTENT_GAP;
	}

	lv_obj_t *container = lv_obj_create(libraryscan_screen);
	lv_obj_set_size(container, lv_pct(100), content_height);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, SCAN_ROW_GAP, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	scan_container = container;

	lv_obj_t *note = lv_image_create(container);
	lv_image_set_src(note, &icon_music_note);
	lv_obj_add_style(note, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(note, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(note, bp_pick(6, 16), 0);
	scan_note = note;

	// The count is the whole point of the page, so it gets the accent colour
	// and the largest type on it.
	count_label = lv_label_create(container);
	lv_label_set_text(count_label, "0");
	lv_obj_set_style_text_color(count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(count_label, &font_ui_32, 0);

	status_label = lv_label_create(container);
	lv_label_set_text(status_label, tr("libraryscan_tracks_found"));
	// Wrapped inside the panel rather than run past it: a label that sizes
	// itself keeps its single line however wide it gets, and a centred flex
	// column then cuts it off at both edges -- which a translation of "tracks
	// found" is long enough to do on 240 px.
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_26, 0);

	progress_label = lv_label_create(container);
	lv_label_set_text(progress_label, "");
	lv_label_set_long_mode(progress_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(progress_label, lv_pct(100));
	lv_obj_add_style(progress_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(progress_label, &font_ui_16, 0);
	lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, 0);

	// Measured with the file name still on screen: it is the last line and the
	// one that was being cut off.
	scan_fit_note();
	lv_obj_set_hidden(progress_label, true);

	// Another face or another text size re-measures every line here, so the
	// note has to give way again.
	fonts_register_change(scan_fit_note);

	// The two buttons share the foot of the page: cancel while the scan runs, OK
	// once it is done.
	cancel_button = make_button(libraryscan_screen, "cancel", lv_color_make(210, 66, 58), cancel_cb);
	ok_button = make_button(libraryscan_screen, "ok", theme()->accent, ok_cb);
	lv_obj_set_hidden(ok_button, true);

	poll_timer = lv_timer_create(poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	// The card may have been written while the player was off.
	lv_timer_t *boot_check = lv_timer_create(boot_check_cb, BOOT_CHECK_DELAY_MS, NULL);
	lv_timer_set_repeat_count(boot_check, 1);
}
