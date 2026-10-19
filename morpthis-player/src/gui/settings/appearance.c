#include "appearance.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"

lv_obj_t *appearance_screen;

static lv_obj_t *theme_buttons[THEME_KIND_COUNT];
static lv_obj_t *style_buttons[THEME_STYLE_COUNT];
static lv_obj_t *btn_boot[BOOTLOGO_RETROSPACE + 1]; // Stock / Retrospace
static lv_obj_t *btn_clock[4]; // left / centre / right / hidden
static lv_obj_t *btn_accent[THEME_ACCENT_COUNT]; // the coloured circles
static lv_obj_t *tint_toggle;
static lv_obj_t *main_menu_icons_toggle;
static lv_obj_t *main_menu_browse_toggle;
static lv_obj_t *battery_percent_toggle;
static lv_obj_t *text_size_pill[FONTS_TEXT_COUNT]; // normal / large / small
static lv_obj_t *theme_buttons[THEME_KIND_COUNT];
// The status bar draws the battery either way; this setting is only about the
// number beside it, and is on by default.
static void battery_percent_cb(lv_event_t *e) {
	(void)e;
	bool shown = lv_obj_has_state(battery_percent_toggle, LV_STATE_CHECKED);
	topbar_set_battery_percent(shown);
	config_set_int("screen", "battery_percent", shown ? 1 : 0);
	config_save();
}

// The accent's hue spread over the backgrounds and cards. theme_set_dynamic_tint
// repaints the interface itself, including this page.
static void dynamic_tint_cb(lv_event_t *e) {
	(void)e;
	theme_set_dynamic_tint(lv_obj_has_state(tint_toggle, LV_STATE_CHECKED));
}

static void main_menu_icons_cb(lv_event_t *e) {
	(void)e;
	main_menu_set_icons_only(lv_obj_has_state(main_menu_icons_toggle, LV_STATE_CHECKED));
}

static void main_menu_browse_cb(lv_event_t *e) {
	(void)e;
	main_menu_set_browse(lv_obj_has_state(main_menu_browse_toggle, LV_STATE_CHECKED));
}

static void refresh_text_size_pills(void) {
	int active = fonts_text_size();
	for (int i = 0; i < FONTS_TEXT_COUNT; i++) {
		settingsrow_pill_active(text_size_pill[i], i == active);
	}
}

// Applied at once, on every page: see fonts_set_text_size().
static void text_size_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int size = (int)(intptr_t)lv_event_get_user_data(e);
	if (!fonts_set_text_size(size)) {
		return;
	}
	config_set_int("ui", "text_size", size);
	config_save();
	refresh_text_size_pills();
}

// ---------------------------------------------------------------------------
// Settings -> Appearance -> Font
//
// The face the whole interface is drawn with, as a row of pills: the families
// in this firmware first (MiSans, and the others its fonts directory carries),
// then whatever the user has put in the Fonts folder on the card. See fonts.c
// for what a choice means and what it costs.
//
// The pills are the family's own names, so they are not translated, and the
// row is built from what is actually there: a family whose files are missing
// is not offered, and a card font appears when the card does -- which is why
// the row is looked at again every time the page is opened.
// ---------------------------------------------------------------------------

// The families in the firmware plus the ones on the card, as fonts_choices()
// reports them: a handful, and the row wraps onto as many lines as it needs
// if there are ever more -- the page scrolls, the row does not.
#define FONT_CHOICES_MAX 24
#define FONT_ID_MAX 160

static lv_obj_t *font_pills;
static lv_obj_t *font_pill[FONT_CHOICES_MAX];
static char font_pill_id[FONT_CHOICES_MAX][FONT_ID_MAX]; // the pill's own copy: fonts_choices() reuses its buffers
static int font_pill_count;

// Paints the chosen family as the filled accent pill, the rest quiet.
static void refresh_font_pills(void) {
	const char *active_id = fonts_choice();
	for (int i = 0; i < font_pill_count; i++) {
		settingsrow_pill_active(font_pill[i], strcmp(font_pill_id[i], active_id) == 0);
	}
}

// The pill's number in the row, one-based, as the pill helpers pass a value
// to their callback: the id itself is looked up here rather than carried as
// the user data, so the list can be rebuilt without leaving a pill pointing
// at a buffer that has moved on.
static void font_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e) - 1;
	if (index < 0 || index >= font_pill_count) {
		return;
	}
	const char *id = font_pill_id[index];
	if (!fonts_set_face(id)) {
		// A card that was pulled out between the page opening and the tap, or
		// a file too large to hold in memory. The row repaints over whatever
		// the failed attempt left behind.
		toast_error("appearance_font_failed");
		refresh_font_pills();
		return;
	}
	config_set("ui", "font", id);
	config_save();
	refresh_font_pills();
}

// Builds the row, and rebuilds it when the list itself has changed: a font
// copied onto the card (or taken off it) appears the next time the page is
// opened, without the pills already there being disturbed in between.
static void font_pills_rebuild(void) {
	const char *ids[FONT_CHOICES_MAX];
	const char *labels[FONT_CHOICES_MAX];
	int count = fonts_choices(ids, labels, FONT_CHOICES_MAX);

	bool same = count == font_pill_count;
	for (int i = 0; same && i < count; i++) {
		same = strcmp(ids[i], font_pill_id[i]) == 0;
	}
	if (same) {
		refresh_font_pills();
		return;
	}

	lv_obj_clean(font_pills);
	font_pill_count = count;
	for (int i = 0; i < count; i++) {
		snprintf(font_pill_id[i], FONT_ID_MAX, "%s", ids[i]);
		font_pill[i] = settingsrow_pill_text(font_pills, labels[i], i + 1, font_pick_cb);
	}
	refresh_font_pills();
}

static void font_pills_reload_cb(lv_event_t *e) {
	(void)e;
	font_pills_rebuild();
}

// Paints one row of choices: the active one outlined, the rest flat.
static void refresh_choice_row(lv_obj_t **buttons, int count, int active_index) {
	for (int i = 0; i < count; i++) {
		lv_obj_t *button = buttons[i];
		if (!button) {
			continue;
		}
		lv_obj_t *label = lv_obj_get_child(button, 0);
		bool active = i == active_index;
		if (active) {
			lv_color_t fill = lv_color_mix(theme()->accent, theme()->surface, (lv_opa_t)64);
			lv_obj_set_style_bg_color(button, fill, 0);
			lv_obj_set_style_text_color(label, theme()->text_primary, 0);
			lv_obj_set_style_border_width(button, 1, 0);
			lv_obj_set_style_border_color(button, theme()->accent, 0);
			lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
		} else {
			lv_obj_set_style_bg_color(button, theme()->surface_pressed, 0);
			lv_obj_set_style_text_color(label, theme()->text_primary, 0);
			lv_obj_set_style_border_width(button, 0, 0);
		}
		lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	}
}

// Paints the active theme choice.
static void refresh_buttons(void) { refresh_choice_row(theme_buttons, THEME_KIND_COUNT, (int)theme_get_kind()); }

static void refresh_style_buttons(void) {
	refresh_choice_row(style_buttons, THEME_STYLE_COUNT, (int)theme_get_style());
}

// The active circle wears a ring in the text colour; the rest sit flat.
static void refresh_accent_buttons(void) {
	int active_accent = theme_get_accent();
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		if (!btn_accent[i]) {
			continue;
		}
		lv_color_t fill = theme_accent_preset(i);
		lv_obj_set_style_bg_color(btn_accent[i], fill, 0);
		bool light_on_light = theme_color_is_light(fill) && theme_color_is_light(theme()->text_primary);
		if (i == active_accent) {
			lv_obj_set_style_border_width(btn_accent[i], 3, 0);
			lv_obj_set_style_border_color(btn_accent[i], light_on_light ? lv_color_black() : theme()->text_primary, 0);
			lv_obj_set_style_border_opa(btn_accent[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_set_style_border_width(btn_accent[i], 0, 0);
		}
		if (i == active_accent && light_on_light) {
			lv_obj_set_style_outline_width(btn_accent[i], 2, 0);
			lv_obj_set_style_outline_pad(btn_accent[i], 0, 0);
			lv_obj_set_style_outline_color(btn_accent[i], theme()->text_primary, 0);
			lv_obj_set_style_outline_opa(btn_accent[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_set_style_outline_width(btn_accent[i], 0, 0);
		}
	}
}

static void accent_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	theme_set_accent(index);
	refresh_accent_buttons();
}

static lv_obj_t *make_accent_circle(lv_obj_t *parent, int index, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, compact ? 30 : 48, compact ? 30 : 48);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, theme_accent_preset(index), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(btn, accent_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	return btn;
}

// Paints the clock-position pills the same way: accent on the active one.
static void refresh_clock_buttons(void) {
	int active = (int)config_get_int("screen", "clock_pos", TOPBAR_CLOCK_CENTER);
	for (int i = 0; i < 4; i++) {
		if (!btn_clock[i]) {
			continue;
		}
		if (i == active) {
			lv_obj_set_style_bg_color(btn_clock[i], theme()->accent, 0);
			lv_obj_set_style_text_color(lv_obj_get_child(btn_clock[i], 0), theme_on_accent(), 0);
		} else {
			lv_obj_set_style_bg_color(btn_clock[i], theme()->surface_pressed, 0);
			lv_obj_set_style_text_color(lv_obj_get_child(btn_clock[i], 0), theme()->text_primary, 0);
		}
	}
}

static void clock_pos_cb(lv_event_t *e) {
	int pos = (int)(intptr_t)lv_event_get_user_data(e);
	config_set_int("screen", "clock_pos", pos);
	config_save();
	topbar_set_clock_position(pos);
	refresh_clock_buttons();
}

static lv_obj_t *make_clock_choice(lv_obj_t *parent, const char *text, int pos, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, compact ? 36 : 64);
	lv_obj_set_style_pad_hor(btn, compact ? 8 : 14, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, clock_pos_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pos);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, compact ? &font_ui_16 : &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static void theme_pick_cb(lv_event_t *e) {
	theme_kind_t kind = (theme_kind_t)(intptr_t)lv_event_get_user_data(e);
	theme_set_kind(kind);
	refresh_buttons();
}

static void style_pick_cb(lv_event_t *e) {
	theme_style_t style = (theme_style_t)(intptr_t)lv_event_get_user_data(e);
	theme_set_style(style);
	refresh_style_buttons();
}

// ---------------------------------------------------------------------------
// The boot screen: the picture the next power-on opens with
//
// Two choices -- Stock and Retrospace -- painted like the theme pair above.
// The choice is the one setting on this page that is not only a config value:
// it also goes into the flash marker the boot script reads, because that
// script runs before any filesystem a config could live on is mounted. What
// is picked here is therefore what the next boot shows, and nothing before it.
// ---------------------------------------------------------------------------

static void refresh_boot_buttons(void) {
	int choice = theme_boot_screen();
	for (int i = 0; i <= BOOTLOGO_RETROSPACE; i++) {
		if (!btn_boot[i]) {
			continue;
		}
		bool on = i == choice;
		lv_obj_set_style_bg_color(btn_boot[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(btn_boot[i], 0),
									on ? theme_on_accent() : theme()->text_primary, 0);
	}
}

static void boot_pick_cb(lv_event_t *e) {
	int choice = (int)(intptr_t)lv_event_get_user_data(e);
	theme_set_boot_screen(choice);
	refresh_boot_buttons();
}

// The two names are not translated, the way a font's name is not: they are
// the names this setting gives the two pictures, and they are the words the
// flash notes and the changelog use.
static lv_obj_t *make_boot_choice(lv_obj_t *parent, const char *text, int choice, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, compact ? 40 : 64);
	lv_obj_set_style_pad_hor(btn, compact ? 14 : 34, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, boot_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)choice);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static lv_obj_t *make_theme_choice(lv_obj_t *parent, const char *text, theme_kind_t kind, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, compact ? lv_pct(48) : LV_SIZE_CONTENT, compact ? 34 : 64);
	lv_obj_set_style_pad_hor(btn, compact ? 5 : 34, 0);
	lv_obj_set_style_radius(btn, compact ? 7 : LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	// Two compact columns leave enough width for each full theme label and
	// avoid flex items painting over one another on the 240 px V1 panel.
	lv_obj_set_flex_grow(btn, 0);
	lv_obj_add_event_cb(btn, theme_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kind);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, compact ? &font_ui_14 : &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static lv_obj_t *make_style_choice(lv_obj_t *parent, const char *text, theme_style_t style, bool compact) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, compact ? lv_pct(48) : LV_SIZE_CONTENT, compact ? 34 : 64);
	lv_obj_set_style_pad_hor(btn, compact ? 5 : 34, 0);
	lv_obj_set_style_radius(btn, compact ? 7 : LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_flex_grow(btn, 0);
	lv_obj_add_event_cb(btn, style_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)style);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, compact ? &font_ui_14 : &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

void appearance_init(gui_config_t *cfg) {
	const bool compact = cfg->screen_width < 320;
	lv_obj_t *container = settingsrow_page(appearance_screen, cfg, "appearance");

	// One tall card: the theme on top, the layout style and its note below it.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, compact ? 8 : 20, 0);
	lv_obj_set_style_pad_gap(card, compact ? 8 : 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("appearance_theme"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, compact ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, compact ? 4 : 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	theme_buttons[THEME_KIND_DARK] = make_theme_choice(row, "appearance_dark", THEME_KIND_DARK, compact);
	theme_buttons[THEME_KIND_LIGHT] = make_theme_choice(row, "appearance_light", THEME_KIND_LIGHT, compact);

	// The layout style: independent of the palette, and the V1's original
	// interface is the default.
	lv_obj_t *style_name = lv_label_create(card);
	lv_label_set_text(style_name, tr("appearance_style"));
	lv_obj_add_style(style_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(style_name, compact ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *style_row = lv_obj_create(card);
	lv_obj_set_size(style_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(style_row, 0, 0);
	lv_obj_set_style_border_width(style_row, 0, 0);
	lv_obj_set_style_pad_all(style_row, 0, 0);
	lv_obj_set_style_pad_gap(style_row, compact ? 4 : 14, 0);
	lv_obj_set_scrollable(style_row, false);
	lv_obj_set_event_bubble(style_row, true);
	lv_obj_set_flex_flow(style_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(style_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	style_buttons[THEME_STYLE_CLASSIC] =
		make_style_choice(style_row, "appearance_style_classic", THEME_STYLE_CLASSIC, compact);
	style_buttons[THEME_STYLE_MORPTHIS] = make_style_choice(style_row, "appearance_style_morpthis", THEME_STYLE_MORPTHIS, compact);

	lv_obj_t *style_note = lv_label_create(card);
	lv_label_set_long_mode(style_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(style_note, lv_pct(100));
	lv_obj_add_style(style_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(style_note, &font_ui_22, 0);
	lv_label_set_text(style_note, tr("appearance_style_note"));

	settingsrow_toggle(container, "appearance_main_menu_icons_only", &main_menu_icons_toggle, main_menu_icons_cb);
	if (main_menu_icons_only()) {
		lv_obj_add_state(main_menu_icons_toggle, LV_STATE_CHECKED);
	}
	lv_obj_t *menu_note = lv_label_create(container);
	lv_label_set_long_mode(menu_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(menu_note, lv_pct(100));
	lv_obj_add_style(menu_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(menu_note, &font_ui_22, 0);
	lv_label_set_text(menu_note, tr("appearance_main_menu_icons_only_note"));

	settingsrow_toggle(container, "appearance_home_browse", &main_menu_browse_toggle, main_menu_browse_cb);
	if (main_menu_browse()) lv_obj_add_state(main_menu_browse_toggle, LV_STATE_CHECKED);

	// A second card: which picture the next power-on shows. Unlike every
	// other choice on this page it is not only a config value -- it also
	// goes into the flash marker the boot script reads, so it takes effect
	// at the next boot and not a moment before.
	lv_obj_t *boot_card = lv_obj_create(container);
	lv_obj_set_width(boot_card, lv_pct(100));
	lv_obj_set_height(boot_card, LV_SIZE_CONTENT);
	lv_obj_add_style(boot_card, &theme_style_card, 0);
	lv_obj_set_style_radius(boot_card, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(boot_card, 0, 0);
	lv_obj_set_style_shadow_width(boot_card, 0, 0);
	lv_obj_set_style_pad_all(boot_card, compact ? 8 : 20, 0);
	lv_obj_set_style_pad_gap(boot_card, compact ? 8 : 18, 0);
	lv_obj_set_scrollable(boot_card, false);
	lv_obj_set_event_bubble(boot_card, true);
	lv_obj_set_flex_flow(boot_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(boot_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *boot_name = lv_label_create(boot_card);
	lv_label_set_text(boot_name, tr("appearance_boot_screen"));
	lv_obj_add_style(boot_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(boot_name, compact ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *boot_row = lv_obj_create(boot_card);
	lv_obj_set_size(boot_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(boot_row, 0, 0);
	lv_obj_set_style_border_width(boot_row, 0, 0);
	lv_obj_set_style_pad_all(boot_row, 0, 0);
	lv_obj_set_style_pad_gap(boot_row, compact ? 6 : 14, 0);
	lv_obj_set_scrollable(boot_row, false);
	lv_obj_set_event_bubble(boot_row, true);
	// Wrapped like the clock row: a pill that does not fit is drawn off the
	// card rather than shrunk.
	lv_obj_set_flex_flow(boot_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(boot_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	btn_boot[BOOTLOGO_STOCK] = make_boot_choice(boot_row, "Stock", BOOTLOGO_STOCK, compact);
	btn_boot[BOOTLOGO_RETROSPACE] = make_boot_choice(boot_row, "Retrospace", BOOTLOGO_RETROSPACE, compact);

	// Under the card, like the other notes on this page: when the change
	// becomes visible.
	lv_obj_t *boot_note = lv_label_create(container);
	lv_label_set_long_mode(boot_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(boot_note, lv_pct(100));
	lv_obj_add_style(boot_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(boot_note, &font_ui_22, 0);
	lv_label_set_text(boot_note, tr("appearance_boot_screen_note"));

	// A third card: where the clock sits in the status bar.
	lv_obj_t *clock_card = lv_obj_create(container);
	lv_obj_set_width(clock_card, lv_pct(100));
	lv_obj_set_height(clock_card, LV_SIZE_CONTENT);
	lv_obj_add_style(clock_card, &theme_style_card, 0);
	lv_obj_set_style_radius(clock_card, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(clock_card, 0, 0);
	lv_obj_set_style_shadow_width(clock_card, 0, 0);
	lv_obj_set_style_pad_all(clock_card, compact ? 8 : 20, 0);
	lv_obj_set_style_pad_gap(clock_card, compact ? 8 : 18, 0);
	lv_obj_set_scrollable(clock_card, false);
	lv_obj_set_event_bubble(clock_card, true);
	lv_obj_set_flex_flow(clock_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clock_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *clock_name = lv_label_create(clock_card);
	lv_label_set_text(clock_name, tr("appearance_clock_position"));
	lv_obj_add_style(clock_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(clock_name, compact ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *clock_row = lv_obj_create(clock_card);
	lv_obj_set_size(clock_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(clock_row, 0, 0);
	lv_obj_set_style_border_width(clock_row, 0, 0);
	lv_obj_set_style_pad_all(clock_row, 0, 0);
	lv_obj_set_style_pad_gap(clock_row, compact ? 4 : 10, 0);
	lv_obj_set_scrollable(clock_row, false);
	lv_obj_set_event_bubble(clock_row, true);
	// Wrapped, not a single line: four pills of translated words do not fit
	// across 480 pixels in every language, and one that does not fit is drawn
	// off the card rather than shrunk.
	lv_obj_set_flex_flow(clock_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(clock_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	btn_clock[TOPBAR_CLOCK_LEFT] = make_clock_choice(clock_row, "left", TOPBAR_CLOCK_LEFT, compact);
	btn_clock[TOPBAR_CLOCK_CENTER] = make_clock_choice(clock_row, "centre", TOPBAR_CLOCK_CENTER, compact);
	btn_clock[TOPBAR_CLOCK_RIGHT] = make_clock_choice(clock_row, "right", TOPBAR_CLOCK_RIGHT, compact);
	btn_clock[TOPBAR_CLOCK_HIDDEN] = make_clock_choice(clock_row, "hide", TOPBAR_CLOCK_HIDDEN, compact);

	// A fourth card: the accent colour, as two rows of coloured circles.
	lv_obj_t *accent_card = lv_obj_create(container);
	lv_obj_set_width(accent_card, lv_pct(100));
	lv_obj_set_height(accent_card, LV_SIZE_CONTENT);
	lv_obj_add_style(accent_card, &theme_style_card, 0);
	lv_obj_set_style_radius(accent_card, compact ? 10 : 12, 0);
	lv_obj_set_style_border_width(accent_card, 0, 0);
	lv_obj_set_style_shadow_width(accent_card, 0, 0);
	lv_obj_set_style_pad_all(accent_card, compact ? 8 : 20, 0);
	lv_obj_set_style_pad_gap(accent_card, compact ? 8 : 18, 0);
	lv_obj_set_scrollable(accent_card, false);
	lv_obj_set_event_bubble(accent_card, true);
	lv_obj_set_flex_flow(accent_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(accent_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *accent_name = lv_label_create(accent_card);
	lv_label_set_text(accent_name, tr("appearance_accent_colour"));
	lv_obj_add_style(accent_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(accent_name, compact ? &font_ui_18 : &font_ui_24, 0);

	lv_obj_t *accent_row = lv_obj_create(accent_card);
	lv_obj_set_size(accent_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(accent_row, 0, 0);
	lv_obj_set_style_border_width(accent_row, 0, 0);
	lv_obj_set_style_pad_all(accent_row, 0, 0);
	// Twelve circles do not fit on one line, so the row wraps onto two of
	// six: at each size the gap is the widest that still keeps six on a line
	// (the row runs 410 and 212 pixels wide on the two panel widths).
	lv_obj_set_style_pad_gap(accent_row, compact ? 4 : 14, 0);
	lv_obj_set_scrollable(accent_row, false);
	lv_obj_set_event_bubble(accent_row, true);
	lv_obj_set_flex_flow(accent_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(accent_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		btn_accent[i] = make_accent_circle(accent_row, i, compact);
	}

	// Straight under the accent, because it is what the tint is made from: the
	// same hue carried across the backgrounds, the cards and the status bar.
	settingsrow_toggle(container, "appearance_dynamic_tint", &tint_toggle, dynamic_tint_cb);
	if (theme_dynamic_tint()) {
		lv_obj_add_state(tint_toggle, LV_STATE_CHECKED);
	}

	lv_obj_t *tint_note = lv_label_create(container);
	lv_label_set_long_mode(tint_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(tint_note, lv_pct(100));
	lv_obj_add_style(tint_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tint_note, &font_ui_22, 0);
	lv_label_set_text(tint_note, tr("appearance_dynamic_tint_note"));

	// How big the text on every page is: secondary lines, notes, clocks and
	// counters. See fonts.c for which sizes change and by how much.
	lv_obj_t *text_size_pills;
	settingsrow_pills(container, "appearance_text_size", &text_size_pills);
	text_size_pill[FONTS_TEXT_SMALL] =
		settingsrow_pill(text_size_pills, "appearance_text_small", FONTS_TEXT_SMALL, text_size_cb);
	text_size_pill[FONTS_TEXT_NORMAL] =
		settingsrow_pill(text_size_pills, "appearance_text_normal", FONTS_TEXT_NORMAL, text_size_cb);
	text_size_pill[FONTS_TEXT_LARGE] =
		settingsrow_pill(text_size_pills, "appearance_text_large", FONTS_TEXT_LARGE, text_size_cb);

	// The face of the whole interface, right under the size of it: the two are
	// the same decision taken twice. The row wraps -- there can be eight
	// families now and a cardful later -- and a family's name is not
	// translated: it is its name.
	settingsrow_pills(container, "appearance_font", &font_pills);

	lv_obj_t *font_note = lv_label_create(container);
	lv_label_set_long_mode(font_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(font_note, lv_pct(100));
	lv_obj_add_style(font_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(font_note, &font_ui_22, 0);
	lv_label_set_text(font_note, tr("appearance_font_note"));

	// A fifth card, and the last plain switch on the page: whether the status
	// bar prints the charge as a number as well as drawing it.
	settingsrow_toggle(container, "appearance_battery_percentage", &battery_percent_toggle, battery_percent_cb);
	if (config_get_int("screen", "battery_percent", 1) != 0) {
		lv_obj_add_state(battery_percent_toggle, LV_STATE_CHECKED);
	}

	font_pills_rebuild();

	refresh_buttons();
	refresh_boot_buttons();
	refresh_clock_buttons();
	refresh_accent_buttons();
	refresh_text_size_pills();
	theme_register_refresh(refresh_buttons);
	theme_register_refresh(refresh_style_buttons);
	theme_register_refresh(refresh_boot_buttons);
	theme_register_refresh(refresh_text_size_pills);
	theme_register_refresh(refresh_clock_buttons);
	theme_register_refresh(refresh_accent_buttons);
	theme_register_refresh(refresh_font_pills);

	// The card's fonts again on every visit: one copied over the USB cable
	// while the player was running is offered the next time this page comes
	// up, and one whose card has been pulled out is gone from the row.
	lv_obj_add_event_cb(appearance_screen, font_pills_reload_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
