#include "chapters.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"

lv_obj_t *chapters_screen;

// Books run to dozens of chapters, occasionally a couple of hundred; the list
// is plainly rebuilt each time it opens, like the audiobook index itself. The
// cap is there for a pathological file, not for any real book.
#define MAX_ROWS 400
#define ROW_HEIGHT bp_pick(48, 68)
#define ROW_RADIUS bp_pick(10, 12)
#define ROW_GAP bp_pick(5, 8)
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)

// The rows that exist. Scrolling moves the body these sit on, so the list does
// not walk four hundred widgets on every step of a drag, and a chapter's title
// is read from the book when its row is first shown rather than when the page
// is opened.
#define ROW_POOL 12

typedef struct {
	lv_obj_t *button;
	lv_obj_t *number;
	lv_obj_t *label;
	lv_obj_t *time_label; // hidden when the entry has no clock of its own
	int index;
} chapter_row_t;

static lv_obj_t *chapter_list;
static lv_obj_t *chapter_body;
static lv_obj_t *empty_label;
static chapter_row_t chapter_rows[ROW_POOL];
static int pool_first = -1;
static int listed_count;
static int playing_index;

// A folder book lists its files here instead: each file is a chapter.
static bool listing_parts;

static void format_clock(double seconds, char *out, size_t out_size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;

	if (hours > 0) {
		snprintf(out, out_size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, out_size, "%ld:%02ld", minutes, secs);
	}
}

static void chapter_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	chapter_row_t *row = lv_event_get_user_data(e);
	if (!row || row->index < 0) {
		return;
	}
	int index = row->index;
	double start = 0;
	if (!audiobook_chapter(index, NULL, 0, &start)) {
		return;
	}

	device_state_seek(start);
	// The position is written straight away rather than at the next ten-second
	// tick: jumping to a chapter and then switching the player off immediately
	// is normal with a book.
	audiobook_note_position(start, 0, true);

	player_refresh_now_playing();
	back_btn_cb(NULL); // back to the player, which is where the jump is heard
}

// Starts part `index` of the folder book from its beginning, with the book's
// parts as the queue again, whatever the queue had become.
static void part_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	chapter_row_t *row = lv_event_get_user_data(e);
	if (!row || row->index < 0) {
		return;
	}
	int index = row->index;
	int count = audiobook_part_count();
	if (index < 0 || index >= count) {
		return;
	}
	char **paths = calloc((size_t)count, sizeof(char *));
	if (!paths) {
		return;
	}
	int filled = 0;
	for (int i = 0; i < count; i++) {
		char path[512];
		if (!audiobook_part(i, NULL, 0, path, sizeof(path)) || !(paths[filled] = strdup(path))) {
			break;
		}
		filled++;
	}
	if (filled == count) {
		device_state_play_list_ordered((const char *const *)paths, count, index);
	}
	for (int i = 0; i < filled; i++) {
		free(paths[i]);
	}
	free(paths);

	player_refresh_now_playing();
	back_btn_cb(NULL);
}

// One handler for the pool, since the same rows list chapters on one book and
// parts on a folder book: which of the two is on screen decides what a row
// means.
static void row_clicked_cb(lv_event_t *e) {
	if (listing_parts) {
		part_clicked_cb(e);
	} else {
		chapter_clicked_cb(e);
	}
}

// Points one pool row at `index` and resolves its title and clock, which is a
// lock and two string copies in the audiobook module.
static void row_bind(chapter_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	if (index < 0 || index >= listed_count) {
		row->index = -1;
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char title[192] = "";
	char clock[24] = "";
	bool has_clock = false;
	if (listing_parts) {
		if (!audiobook_part(index, title, sizeof(title), NULL, 0)) {
			row->index = -1;
			lv_obj_set_hidden(row->button, true);
			return;
		}
	} else {
		double start = 0;
		if (!audiobook_chapter(index, title, sizeof(title), &start)) {
			row->index = -1;
			lv_obj_set_hidden(row->button, true);
			return;
		}
		format_clock(start, clock, sizeof(clock));
		has_clock = true;
	}

	row->index = index;
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text_fmt(row->number, "%d", index + 1);
	lv_label_set_text(row->label, title[0] ? title : tr("chapter"));
	if (has_clock) {
		lv_label_set_text(row->time_label, clock);
		lv_obj_set_hidden(row->time_label, false);
	} else {
		lv_obj_set_hidden(row->time_label, true);
	}

	// The chapter being listened to is marked the way the queue marks the
	// playing track: the accent on the words, not a second widget. A row that
	// moves from one chapter to another has to give the accent back.
	if (index == playing_index) {
		lv_color_t accent = theme()->accent;
		lv_obj_set_style_text_color(row->number, accent, 0);
		lv_obj_set_style_text_color(row->label, accent, 0);
		lv_obj_set_style_text_color(row->time_label, accent, 0);
	} else {
		lv_obj_remove_local_style_prop(row->number, LV_STYLE_TEXT_COLOR, 0);
		lv_obj_remove_local_style_prop(row->label, LV_STYLE_TEXT_COLOR, 0);
		lv_obj_remove_local_style_prop(row->time_label, LV_STYLE_TEXT_COLOR, 0);
	}
}

static int pool_first_for(void) {
	if (listed_count <= 0) {
		return 0;
	}
	int scroll = lv_obj_get_scroll_y(chapter_list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = scroll / ROW_PITCH - 1;
	if (first + ROW_POOL > listed_count) {
		first = listed_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	return first;
}

static void window_update(void) {
	int first = pool_first_for();
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&chapter_rows[index % ROW_POOL], index < listed_count ? index : -1);
	}
	pool_first = first;
}

static void list_scroll_cb(lv_event_t *e) {
	(void)e;
	if (pool_first_for() != pool_first) {
		window_update();
	}
}

static void row_create(chapter_row_t *row, lv_event_cb_t cb) {
	row->index = -2;

	row->button = lv_btn_create(chapter_body);
	lv_obj_set_size(row->button, lv_pct(100), ROW_HEIGHT);
	lv_obj_set_x(row->button, 0);
	lv_obj_add_style(row->button, &theme_style_card, 0);
	lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row->button, 0, 0);
	lv_obj_set_style_shadow_width(row->button, 0, 0);
	lv_obj_set_style_pad_all(row->button, bp_pick(8, 14), 0);
	lv_obj_set_style_pad_column(row->button, bp_pick(8, 12), 0);
	lv_obj_set_event_bubble(row->button, true);
	lv_obj_set_hidden(row->button, true);
	lv_obj_add_event_cb(row->button, cb, LV_EVENT_CLICKED, row);
	lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	row->number = lv_label_create(row->button);
	lv_obj_set_width(row->number, bp_pick(30, 44));
	lv_obj_set_style_text_align(row->number, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_add_style(row->number, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->number, &font_ui_24, 0);

	row->label = lv_label_create(row->button);
	lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(row->label, 1);
	// One line, then dots. Without a height the label wraps instead, and a
	// long chapter name then grows past the fixed row it lives in.
	lv_obj_set_height(row->label, 32);
	lv_obj_add_style(row->label, &theme_style_text, 0);
	lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

	row->time_label = lv_label_create(row->button);
	lv_obj_add_style(row->time_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->time_label, &font_ui_18, 0);
	lv_obj_set_hidden(row->time_label, true);
}

// The row to open the list on: the chapter, or the part, being listened to.
static int playing_row(void) {
	if (listing_parts) {
		return audiobook_part_current();
	}
	device_state_t state;
	device_state_get(&state);
	return audiobook_chapter_at(state.progress_current_secs);
}

static void empty_show(void) {
	if (empty_label) {
		return;
	}
	empty_label = lv_label_create(chapter_list);
	lv_label_set_text(empty_label, tr("chapters_empty"));
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(empty_label, lv_pct(100));
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_set_style_pad_top(empty_label, 80, 0);
}

static void rebuild(void) {
	if (empty_label) {
		lv_obj_delete(empty_label);
		empty_label = NULL;
	}

	listing_parts = audiobook_part_count() > 1;

	int count;
	if (listing_parts) {
		count = audiobook_part_count();
	} else {
		count = audiobook_chapter_count();
	}
	if (count > MAX_ROWS) {
		count = MAX_ROWS;
	}
	if (count < 0) {
		count = 0;
	}
	listed_count = count;
	playing_index = count > 0 ? playing_row() : -1;

	for (int i = 0; i < ROW_POOL; i++) {
		chapter_rows[i].index = -2; // every row stands for a different chapter now
		lv_obj_set_hidden(chapter_rows[i].button, true);
	}
	pool_first = -1;

	lv_obj_set_height(chapter_body, count > 0 ? count * ROW_PITCH - ROW_GAP : 0);
	lv_obj_scroll_to_y(chapter_list, 0, LV_ANIM_OFF);
	window_update();

	if (count == 0) {
		empty_show();
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();

	// Open on the chapter being listened to rather than at the top: on a book
	// with sixty chapters, the one that matters is the one playing. The scroll
	// is by position rather than by widget: the widget for that chapter may not
	// have been built yet.
	if (playing_index > 0 && playing_index < listed_count) {
		int32_t y = playing_index * ROW_PITCH;
		int32_t view_h = lv_obj_get_height(chapter_list);
		int32_t target = y + ROW_HEIGHT - view_h;
		if (target < 0 || target > y) {
			target = y;
		}
		lv_obj_scroll_to_y(chapter_list, target, LV_ANIM_OFF);
		window_update();
	}
}

void chapters_open(void) {
	player_sheet_close();
	switch_screen(chapters_screen);
	switcher_set_player_return(chapters_screen);
}

void chapters_init(gui_config_t *cfg) {
	lv_obj_add_style(chapters_screen, &theme_style_screen, 0);
	settingsrow_title(chapters_screen, cfg, "chapters");

	int content_top = settingsrow_content_top(cfg);

	chapter_list = lv_obj_create(chapters_screen);
	lv_obj_set_size(chapter_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(chapter_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(chapter_list, 0, 0);
	lv_obj_set_style_border_width(chapter_list, 0, 0);
	lv_obj_set_style_radius(chapter_list, 0, 0);
	lv_obj_set_style_pad_hor(chapter_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(chapter_list, 0, 0);
	lv_obj_set_scroll_dir(chapter_list, LV_DIR_VER);
	// Set here, not in the row builder: a book with no chapters creates no
	// rows, and the page still has to answer the swipe-back.
	lv_obj_set_event_bubble(chapter_list, true);

	// Everything the list scrolls. The scroller has one child of its own, so a
	// scroll step moves it and the rows stay where they are inside it.
	chapter_body = lv_obj_create(chapter_list);
	lv_obj_remove_style_all(chapter_body);
	lv_obj_set_width(chapter_body, lv_pct(100));
	lv_obj_set_height(chapter_body, 0);
	lv_obj_set_scrollable(chapter_body, false);
	lv_obj_set_clickable(chapter_body, false);

	for (int i = 0; i < ROW_POOL; i++) {
		row_create(&chapter_rows[i], row_clicked_cb);
	}
	lv_obj_add_event_cb(chapter_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	// No player-sheet swipe here, for the queue page's reason: it would slide
	// the sheet over the list being read. The swipe-back still works.
	switcher_attach_back_gesture(chapter_list);
	lv_obj_add_event_cb(chapters_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
