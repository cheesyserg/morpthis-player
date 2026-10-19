#include "systempage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/easteregg.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/fwupdate.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/device/factoryreset.h"
#include "src/system/audio/headset.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"

lv_obj_t *systempage_screen;
lv_obj_t *sysinfo_screen;

// The headset controls are under Settings > More (see settings.c). System is
// entered to update the firmware or wipe the device, twice a year, which has
// nothing to do with a switch touched when headphones change.

// ---------------------------------------------------------------------------
// Firmware update
//
//   battery below 30% and not charging -> refuse, and say why
//   otherwise                          -> the update card (fwupdate.h): from
//                                         the internet or from the card
// ---------------------------------------------------------------------------

#define FIRMWARE_MIN_BATTERY_PERCENT 30

static void firmware_clicked_cb(lv_event_t *e) {
	(void)e;

	// Charge check first, like the stock player: an update that loses power
	// halfway leaves the device in recovery with a half-written kernel, the one
	// failure here that cannot be undone from the UI. An unreadable level must
	// not block the update: the reader returns "!!" when it cannot find the
	// gauge (the host build, and any firmware naming that sysfs node
	// differently), and refusing to update over a battery nobody can measure
	// would be the opposite of useful.
	char *percent_text = read_battery_percent();
	int percent = 100;
	if (percent_text && percent_text[0] >= '0' && percent_text[0] <= '9') {
		percent = atoi(percent_text);
	}
	bool charging = read_battery_charging();

	if (percent < FIRMWARE_MIN_BATTERY_PERCENT && !charging) {
		gui_notify_popup("system_battery_warning");
		return;
	}

	fwupdate_show();
}

// ---------------------------------------------------------------------------
// Factory reset
//
// Two confirmations in a row, like iOS and Android. Not a formality: the first
// asks whether to do it, the second says what is lost, and they are separate
// deliberately, because the second only appears after the first has closed, so
// they cannot be tapped through by accident.
// ---------------------------------------------------------------------------

static void factory_second_confirmed(void *user) {
	(void)user;
	// Point of no return: factoryreset_run() does not return, it reboots.
	factoryreset_run();
}

static void factory_first_confirmed(void *user) {
	(void)user;
	confirm_show("system_are_you_sure", "system_reset_confirm_note",
				 "system_reset_2", factory_second_confirmed, NULL);
}

static void factory_clicked_cb(lv_event_t *e) {
	(void)e;
	confirm_show("system_factory_reset", "system_reset_confirm",
				 "system_continue", factory_first_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

// The Adwaita red, from the same palette as the rest of the interface.
#define BAR_RED lv_color_make(224, 27, 36)

// Below this share of free space the bar turns red.
#define SD_LOW_PERCENT 10

static lv_obj_t *sd_value;
static lv_obj_t *sd_bar;

static void sd_refresh(void) {
	if (!sd_value || !sd_bar) {
		return;
	}

	sysinfo_storage_t usage;
	if (!sysinfo_sd_usage(&usage) || !usage.present) {
		lv_label_set_text(sd_value, tr("system_no_card"));
		lv_obj_set_hidden(sd_bar, true);
		return;
	}

	char used[32], total[32], text[160];
	sysinfo_format_size(usage.used, used, sizeof(used));
	sysinfo_format_size(usage.total, total, sizeof(total));
	// A read-only card is worth saying here rather than leaving the user to
	// discover it as five separate features that quietly do nothing.
	if (storage_sd_writable()) {
		snprintf(text, sizeof(text), "%s / %s", used, total);
	} else {
		snprintf(text, sizeof(text), "%s / %s  (%s)", used, total, tr("system_read_only"));
	}
	lv_label_set_text(sd_value, text);

	lv_obj_set_hidden(sd_bar, false);
	lv_bar_set_value(sd_bar, usage.used_percent, LV_ANIM_OFF);

	// Red once free space drops below the threshold. The comparison is integer
	// and multiplies instead of dividing: a percentage computed first would
	// lose exactly the borderline cases, and on a two-terabyte card free * 100
	// still fits comfortably in 64 bits.
	bool low = usage.free * 100 <= usage.total * SD_LOW_PERCENT;
	lv_obj_set_style_bg_color(sd_bar, low ? BAR_RED : theme()->accent, LV_PART_INDICATOR);
}

// ---------------------------------------------------------------------------
// The two versions, and the five taps
//
// As on Android: developer options stay hidden until the build number is tapped
// five times. Not security -- anyone reading this knows how -- but keeping a
// page that serves two people out of everyone else's way.
// ---------------------------------------------------------------------------

#define DEVOPTIONS_TAPS 5

// The tap from which the remaining count is announced. Nothing is said before
// that, or there would be no discovery left.
#define DEVOPTIONS_HINT_FROM 3

static int build_taps;

bool systempage_devoptions_unlocked(void) { return config_get_int("system", "devoptions_unlocked", 0) != 0; }

static void build_tapped_cb(lv_event_t *e) {
	(void)e;

	if (systempage_devoptions_unlocked()) {
		gui_notify_popup("system_devoptions_already_on");
		return;
	}

	build_taps++;
	int missing = DEVOPTIONS_TAPS - build_taps;

	if (missing > 0) {
		if (build_taps >= DEVOPTIONS_HINT_FROM) {
			char text[96];
			snprintf(text, sizeof(text), tr("system_devoptions_countdown"), missing);
			gui_notify_popup(text);
		}
		return;
	}

	config_set_int("system", "devoptions_unlocked", 1);
	config_save();
	build_taps = 0;

	settings_refresh_devoptions();
	gui_notify_popup("system_devoptions_on");
}

// ---------------------------------------------------------------------------
// building the two pages
// ---------------------------------------------------------------------------

// The V1 has 240 pixels across: a name beside a value leaves too little room
// for real DAC names, serials and translated labels. Stack the name and value
// on their own lines there, while leaving the regular players' shared settings
// row untouched. The single-line labels use dots rather than running beyond
// the card when a firmware or translation supplies a long value.
static lv_obj_t *compact_info_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out,
							  lv_event_cb_t callback, void *user_data, bool clickable) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 48);
	lv_obj_add_style(row, &theme_style_card, 0);
	if (clickable) {
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	}
	lv_obj_set_style_radius(row, bp_tile_radius(), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 8, 0);
	lv_obj_set_style_pad_ver(row, 4, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_clickable(row, clickable);
	if (callback) {
		lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
	}

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(name));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_14, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_14));
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *value = lv_label_create(row);
	lv_obj_add_style(value, &theme_style_text, 0);
	lv_obj_set_style_text_font(value, &font_ui_20, 0);
	lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
	lv_obj_set_width(value, lv_pct(100));
	lv_obj_set_height(value, lv_font_get_line_height(&font_ui_20));
	lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	if (value_out) {
		*value_out = value;
	}

	return row;
}

// A read-only card with the name on the left and value on the right on the
// larger players. On the V1, `compact_info_row()` stacks them to keep both
// readable within the narrow panel.
static lv_obj_t *info_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out) {
	if (bp_is_tempotec_v1()) {
		return compact_info_row(parent, name, value_out, NULL, NULL, false);
	}

	lv_obj_t *row = settingsrow_add(parent, name, value_out, NULL, NULL);
	lv_obj_set_clickable(row, false);
	return row;
}

// The credit values (a person's name) must stay whole: an ellipsis in a name
// is worse than a smaller one. On the V1 the value line is 20 px in a space
// whose width is already known from the config, so the size steps down
// through the same sizes fit_row_label() steps its names through, and the
// last one fits any face the player ships.
static void credit_value_fit(lv_obj_t *value, int content_width) {
	static const lv_font_t *const STEPS[] = {&font_ui_20, &font_ui_18, &font_ui_16, &font_ui_14};
	const size_t count = sizeof(STEPS) / sizeof(STEPS[0]);

	const char *text = lv_label_get_text(value);
	if (!text || !text[0] || content_width <= 0) {
		return;
	}

	for (size_t i = 0; i < count; i++) {
		lv_point_t size;
		lv_text_get_size(&size, text, STEPS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= content_width || i + 1 == count) {
			// Only when it actually changes: setting the same font again
			// retriggers layout.
			if (lv_obj_get_style_text_font(value, LV_PART_MAIN) != STEPS[i]) {
				lv_obj_set_style_text_font(value, STEPS[i], 0);
			}
			return;
		}
	}
}

static lv_obj_t *system_value_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out,
								  lv_event_cb_t callback, void *user_data, bool clickable) {
	if (bp_is_tempotec_v1()) {
		return compact_info_row(parent, name, value_out, callback, user_data, clickable);
	}

	lv_obj_t *row = settingsrow_add(parent, name, value_out, callback, user_data);
	if (!clickable) {
		lv_obj_set_clickable(row, false);
	}
	return row;
}

// Percentage widths are resolved by the list layout. Refresh dotted labels
// once that layout has settled so an early, zero-width measurement cannot leave
// a translated label displaying only its ellipsis.
static void refresh_dotted_labels(lv_obj_t *obj) {
	if (lv_obj_check_type(obj, &lv_label_class) && lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT) {
		lv_label_set_text(obj, NULL);
	}

	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		refresh_dotted_labels(lv_obj_get_child(obj, (int32_t)i));
	}
}

static void sysinfo_loaded_cb(lv_event_t *e) {
	(void)e;
	// Card usage changes while the player runs, so it is re-read on every
	// opening, and the tap counters start over.
	sd_refresh();
	build_taps = 0;
	easteregg_reset();
}

// The usage bar is redrawn with the new palette.
static void refresh_theme(void) { sd_refresh(); }

static void build_sysinfo_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(sysinfo_screen, cfg, "system_about");

	// --- device and DAC: two read-only rows ---
	// Both come from system-info.json, which is the one place that knows which
	// player this is; neither goes through tr(), a device name and a silicon
	// part number being the same in every language.
	lv_obj_t *device_value = NULL;
	info_row(container, "system_device", &device_value);
	const char *device = sysinfo_device_name();
	lv_label_set_text(device_value, device[0] ? device : "\xE2\x80\x94");

	lv_obj_t *dac_value = NULL;
	info_row(container, "dac", &dac_value);
	const char *dac = sysinfo_dac_info();
	lv_label_set_text(dac_value, dac[0] ? dac : "\xE2\x80\x94");

	// The serial number, from the SoC efuse: the same one printed on the box
	// ("R3PII" plus the first eight hex digits of the chip id). Not translated,
	// being an identifier.
	lv_obj_t *serial_value = NULL;
	info_row(container, "system_serial_number", &serial_value);
	const char *sn = sysinfo_serial_number();
	lv_label_set_text(serial_value, sn[0] ? sn : "\xE2\x80\x94");

	// --- microSD card ---
	// On the narrow V1, the amount sits on its own line instead of fighting
	// the heading for the same 212 pixels. The larger layout keeps its original
	// side-by-side treatment.
	const bool compact = bp_is_tempotec_v1();
	lv_obj_t *sd_card = lv_obj_create(container);
	lv_obj_set_width(sd_card, lv_pct(100));
	lv_obj_set_height(sd_card, bp_pick(60, 116));
	lv_obj_add_style(sd_card, &theme_style_card, 0);
	lv_obj_set_style_radius(sd_card, bp_pick(bp_tile_radius(), 12), 0);
	lv_obj_set_style_border_width(sd_card, 0, 0);
	lv_obj_set_style_shadow_width(sd_card, 0, 0);
	lv_obj_set_style_pad_hor(sd_card, bp_pick(8, 20), 0);
	lv_obj_set_style_pad_ver(sd_card, bp_pick(6, 16), 0);
	lv_obj_set_scrollable(sd_card, false);
	lv_obj_set_clickable(sd_card, false);

	lv_obj_t *sd_name = lv_label_create(sd_card);
	lv_label_set_text(sd_name, tr("system_sd_card"));
	lv_obj_add_style(sd_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(sd_name, compact ? &font_ui_14 : &font_ui_24, 0);
	if (compact) {
		lv_obj_set_width(sd_name, lv_pct(100));
		lv_label_set_long_mode(sd_name, LV_LABEL_LONG_DOT);
	}
	lv_obj_align(sd_name, LV_ALIGN_TOP_LEFT, 0, 0);

	sd_value = lv_label_create(sd_card);
	lv_obj_add_style(sd_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sd_value, compact ? &font_ui_16 : &font_ui_20, 0);
	if (compact) {
		lv_obj_set_width(sd_value, lv_pct(100));
		lv_label_set_long_mode(sd_value, LV_LABEL_LONG_DOT);
		lv_obj_align(sd_value, LV_ALIGN_TOP_LEFT, 0, lv_font_get_line_height(&font_ui_14) + 3);
	} else {
		lv_obj_align(sd_value, LV_ALIGN_TOP_RIGHT, 0, 0);
	}
	lv_label_set_text(sd_value, "");

	sd_bar = lv_bar_create(sd_card);
	lv_obj_set_width(sd_bar, lv_pct(100));
	lv_obj_set_height(sd_bar, bp_pick(5, 10));
	lv_obj_align(sd_bar, LV_ALIGN_BOTTOM_MID, 0, compact ? -1 : 0);
	lv_bar_set_range(sd_bar, 0, 100);
	lv_obj_set_style_radius(sd_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(sd_bar, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sd_bar, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(sd_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(sd_bar, theme()->accent, LV_PART_INDICATOR);

	// --- the two versions ---
	// The system version carries the thank-you (see easteregg.c), so it stays
	// clickable where the rows above it are not.
	lv_obj_t *os_value = NULL;
	lv_obj_t *os_row = system_value_row(container, "system_operating_system_version", &os_value, NULL, NULL, true);
	const char *os = sysinfo_os_version();
	const char *build = sysinfo_build_version();
	char os_display[128];
	// The V1 release channel is stored as "beta" in system-info.json.
	snprintf(os_display, sizeof(os_display), "%s", os);
	lv_label_set_text(os_value, os_display[0] ? os_display : "\xE2\x80\x94"); // em dash
	easteregg_attach(os_row);

	// Five taps reveal the developer options.
	lv_obj_t *build_value = NULL;
	system_value_row(container, "system_build_number", &build_value, build_tapped_cb, NULL, true);
	lv_label_set_text(build_value, build[0] ? build : "\xE2\x80\x94");

	// --- the credits ---
	// Proper names, the same in every language, so they come in as written and
	// never go through tr(): Jepl4r wrote the original Sonix Player for the
	// HiBy players, and this fork carries it to the TempoTec V1.
	//
	// How wide a value line on this page can be: the screen, the page padding
	// on both sides and the row's own. Only the V1 needs it.
	const int value_width = compact ? cfg->screen_width - 2 * cfg->padding - 16 : 0;

	lv_obj_t *original_author_value = NULL;
	info_row(container, "system_original_author", &original_author_value);
	lv_label_set_text(original_author_value, "Jepl4r");
	if (compact) {
		credit_value_fit(original_author_value, value_width);
	}

	lv_obj_t *forked_by_value = NULL;
	info_row(container, "system_forked_by", &forked_by_value);
	lv_label_set_text(forked_by_value, "Imre Ferenczi (mr-f0xx)");
	if (compact) {
		credit_value_fit(forked_by_value, value_width);
	}

	if (compact) {
		lv_obj_update_layout(container);
		refresh_dotted_labels(container);
	}

	lv_obj_add_event_cb(sysinfo_screen, sysinfo_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(sysinfo_screen);
	theme_register_refresh(refresh_theme);
}

void systempage_init(gui_config_t *cfg) {
	// system-info.json was read at startup, before the display: the simulator
	// sizes its window from the model named in it.
	build_sysinfo_page(cfg);

	lv_obj_t *container = settingsrow_page(systempage_screen, cfg, "system");

	settingsrow_add(container, "system_about", NULL, switch_screen_cb, sysinfo_screen);
	// Actions, not pages: settingsrow_action leaves off the chevron.
	settingsrow_action(container, "system_update_firmware", firmware_clicked_cb, NULL);
	settingsrow_action(container, "system_factory_reset", factory_clicked_cb, NULL);

	switcher_attach_back_gesture(systempage_screen);
}
