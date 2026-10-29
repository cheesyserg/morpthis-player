#include "touchcalpage.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/touchcal.h"

lv_obj_t *touchcalpage_screen;

static lv_obj_t *hint_label;
static lv_obj_t *status_label;
static lv_timer_t *refresh_timer;
static int32_t extent;

// A message from Save or Restore stays long enough to read; the live count
// takes over again afterwards.
#define MESSAGE_MS 2500
static uint32_t message_at;

static bool message_holding(void) {
	return message_at != 0 && lv_tick_elaps(message_at) < MESSAGE_MS;
}

static void hold_message(void) {
	message_at = lv_tick_get();
	if (message_at == 0) {
		message_at = 1; // 0 is "no message"; a tick of 0 must not collide with it
	}
}

static void show_current(void) {
	lv_label_set_text_fmt(hint_label, "Using: %d / %d (gap %d)\n"
									  "Swipe slowly up and down, then lift. Repeat.",
						  touchcal_low(), touchcal_high(), touchcal_gap());
}

static void update_status(void) {
	if (message_holding()) {
		return;
	}

	int32_t lo, hi, gap;
	if (touchcal_result(&lo, &hi, &gap)) {
		lv_label_set_text_fmt(status_label, "Measured: low %d, high %d, gap %d. Tap Save.", lo, hi, gap);
	} else {
		uint32_t have = touchcal_samples();
		uint32_t need = touchcal_needed();
		if (have > need) {
			have = need;
		}
		lv_label_set_text_fmt(status_label, "Crossings: %u of %u", have, need);
	}
}

static void refresh_cb(lv_timer_t *timer) {
	(void)timer;
	update_status();
}

static void save_cb(lv_event_t *e) {
	(void)e;

	int32_t lo, hi, gap;
	if (!touchcal_result(&lo, &hi, &gap)) {
		lv_label_set_text(status_label, "Not enough crossings yet.");
		hold_message();
		return;
	}
	if (!touchcal_apply(lo, hi)) {
		lv_label_set_text(status_label, "Those values are not usable.");
		hold_message();
		return;
	}

	lv_label_set_text_fmt(status_label, "Saved: low %d, high %d, gap %d.", lo, hi, gap);
	hold_message();
	show_current();
}

static void reset_cb(lv_event_t *e) {
	(void)e;

	touchcal_defaults();
	touchcal_begin(extent); // the old samples measured the old panel; start over

	show_current();
	lv_label_set_text_fmt(status_label, "Defaults restored: low %d, high %d.", touchcal_low(), touchcal_high());
	hold_message();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	extent = 319;
	lv_display_t *disp = lv_display_get_default();
	if (disp) {
		extent = (int32_t)lv_display_get_vertical_resolution(disp) - 1;
	}
	touchcal_begin(extent);
	message_at = 0;
	show_current();
	update_status();
	lv_timer_resume(refresh_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	touchcal_end();
	lv_timer_pause(refresh_timer);
}

void touchcalpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(touchcalpage_screen, cfg, "Touch cal");

	// The page is the calibration surface. The touch wrapper sees the raw
	// coordinate before LVGL does either way, so a vertical drag is measured
	// whether or not the page scrolls; not scrolling keeps the text still
	// under the finger while it is.
	lv_obj_set_scrollable(container, false);

	hint_label = lv_label_create(container);
	lv_obj_set_width(hint_label, lv_pct(100));
	lv_obj_set_height(hint_label, 64); // three lines at 16 px, fixed: the rows below must not move
	lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(hint_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hint_label, &font_ui_16, 0);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_height(status_label, 48); // two lines at 18 px
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(status_label, &font_ui_18, 0);

	settingsrow_action(container, "Save", save_cb, NULL);
	settingsrow_action(container, "Restore defaults", reset_cb, NULL);

	refresh_timer = lv_timer_create(refresh_cb, 200, NULL);
	lv_timer_pause(refresh_timer);

	lv_obj_add_event_cb(touchcalpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(touchcalpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	show_current();
	update_status();
}
