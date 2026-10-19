#include "main_menu.h"

#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu_icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"

lv_obj_t *main_menu_screen;

static lv_obj_t *menu_grid;
static lv_obj_t *home_header;
static lv_obj_t *home_title;
static lv_obj_t *home_note;
static lv_obj_t *mini_player;
static lv_obj_t *mini_player_halo;
static lv_obj_t *mini_player_icon;
static lv_obj_t *mini_player_copy;
static lv_obj_t *mini_player_title;
static lv_obj_t *mini_player_subtitle;
static lv_timer_t *mini_player_timer;
static gui_config_t *menu_cfg;
static void main_menu_refresh_tiles(void);

// lv_btn supplies state-specific focus and shadow styles. Those do not inherit
// the zero-width default style below, and on the compact footer their bottom
// edge is visible as the unwanted grey/cyan bar. Keep the footer flat in every
// state while retaining its normal pressed fill.
static void mini_player_clear_edge(void) {
	if (!mini_player) {
		return;
	}

	static const lv_state_t states[] = {
		LV_STATE_DEFAULT,
		LV_STATE_PRESSED,
		LV_STATE_FOCUSED,
		LV_STATE_FOCUSED | LV_STATE_PRESSED,
		LV_STATE_FOCUS_KEY,
		LV_STATE_FOCUS_KEY | LV_STATE_PRESSED,
	};
	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		lv_style_selector_t selector = LV_PART_MAIN | states[i];
		lv_obj_set_style_border_width(mini_player, 0, selector);
		lv_obj_set_style_border_opa(mini_player, LV_OPA_TRANSP, selector);
		lv_obj_set_style_outline_width(mini_player, 0, selector);
		lv_obj_set_style_outline_opa(mini_player, LV_OPA_TRANSP, selector);
		lv_obj_set_style_shadow_width(mini_player, 0, selector);
		lv_obj_set_style_shadow_opa(mini_player, LV_OPA_TRANSP, selector);
	}
}

static const lv_image_dsc_t *const NORMAL_MENU_ICONS[6] = {
	&icon_menu_music,
	&icon_menu_streaming,
	&icon_menu_wireless,
	&icon_menu_audiobooks,
	&icon_menu_more,
	&icon_menu_settings,
};

static const lv_image_dsc_t *const TOKYO_MENU_ICONS[6] = {
	&icon_music2,           // Music
	&icon_radio_player,     // Streaming
	&icon_wifi_high,        // Wireless
	&icon_files_audiobook,  // Audiobooks
	&icon_ellipsis_vertical, // More
	&icon_music_settings,   // Settings
};

static const theme_semantic_t TOKYO_MENU_TONES[6] = {
	THEME_SEMANTIC_CYAN,
	THEME_SEMANTIC_PURPLE,
	THEME_SEMANTIC_BLUE,
	THEME_SEMANTIC_AMBER,
	THEME_SEMANTIC_GREEN,
	THEME_SEMANTIC_MUTED,
};

// Audiobooks reads its library straight off the card, so it is the one tile
// that opens onto nothing when the card is out or at a computer. Music has the
// same rule a level deeper, where a list is asked for.
static void open_audiobooks(void) {
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	switch_screen(audiobooks_screen);
}

static void fit_menu_icon(lv_obj_t *image, const lv_image_dsc_t *source, int size) {
	if (!image || !source) {
		return;
	}
	int max_side = LV_MAX((int)source->header.w, (int)source->header.h);
	if (max_side <= 0) {
		return;
	}
	if (size <= 0) {
		size = max_side;
	}
	lv_obj_set_size(image, LV_MAX(1, (int)source->header.w * size / max_side),
					LV_MAX(1, (int)source->header.h * size / max_side));
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(image, (uint32_t)(LV_SCALE_NONE * size / max_side));
}

static void set_label_if_changed(lv_obj_t *label, const char *text) {
	if (!label || !text) {
		return;
	}
	const char *current = lv_label_get_text(label);
	if (!current || strcmp(current, text) != 0) {
		lv_label_set_text(label, text);
	}
}

// The V1 home's title and tagline. OLED leaves both out: its grid starts right
// under the status bar, and the six cards share the height the header took.
#define HOME_HEADER_HEIGHT 62

static bool home_header_shown(void) { return !theme_is_oled(); }

bool main_menu_icons_only(void) { return config_get_bool("ui", "main_menu_icons_only", false); }

// Whether Browse takes the Music slot on the home screen.
bool main_menu_browse(void) { return config_get_bool("ui", "home_browse", false); }

void main_menu_set_browse(bool enabled) {
	if (main_menu_browse() == enabled) {
		return;
	}
	config_set_bool("ui", "home_browse", enabled);
	config_save();
	main_menu_refresh_tiles();
}

static void open_home_music(void) {
	if (main_menu_browse()) {
		// Straight into the card's Music folder, rather than the card root the
		// Browse tile used to show.
		browser_start_in_music_folder();
		switch_screen(browser_screen);
	} else {
		switch_screen(music_screen);
	}
}

// Only this grid opts into hidden captions. Reapply after its geometry changes
// so the larger icons fit the space left by each theme, header and footer.
static void main_menu_refresh_tiles(void) {
	if (!menu_grid || !menu_cfg) {
		return;
	}

	bool compact = bp_is_tempotec_v1();
	bool tokyo = theme_uses_function_glyphs();
	bool icons_only = main_menu_icons_only();
	bool browse = main_menu_browse();
	gridpage_set_tile(menu_grid, 0, browse ? &icon_menu_explorer : &icon_menu_music,
					 browse ? "music_browse" : "music");
	bool horizontal = compact && tokyo && !icons_only;
	for (int i = 0; i < 6; i++) {
		lv_obj_t *tile = lv_obj_get_child(menu_grid, i);
		int icon_size = tokyo ? (compact ? 22 : 46) : (compact ? 46 : 0);
		if (icons_only) {
			// Leave breathing room inside the tile. Size both the image's
			// drawing and its flex layout box.
			icon_size = LV_MAX(1, LV_MIN(lv_obj_get_width(tile), lv_obj_get_height(tile)) - bp_pick(12, 24));
			// OLED's header-free grid has taller tiles. Leave more breathing
			// room for its glyphs without changing the grid or touch targets.
			if (theme_is_oled()) {
				icon_size = LV_MAX(1, icon_size * 4 / 5);
			}
			// A further 5% reduction in every theme, rounded to the nearest
			// display pixel so compact glyphs do not shrink by a full extra pixel.
			icon_size = LV_MAX(1, (icon_size * 95 + 50) / 100);
		}

		const lv_image_dsc_t *source = tokyo ? TOKYO_MENU_ICONS[i] : NORMAL_MENU_ICONS[i];
		if (i == 0 && browse) source = tokyo ? &icon_folder : &icon_menu_explorer;
		if (icons_only && !(i == 0 && browse)) {
			// Use art pre-rasterized at the final pixel size, not stretched
			// toolbar glyphs or colored art filtered for a second time.
			// Matching source/layout sizes also avoids LVGL's fractional image
			// transform and its extra edge interpolation. Other menus and the
			// labeled layout continue to use their original sources and sizes.
			const lv_image_dsc_t *native = main_menu_icon_native(i, tokyo, icon_size);
			if (native) {
				source = native;
			}
		}
		gridpage_set_tile_label_hidden(menu_grid, i, icons_only);
		if (tokyo) {
			// Keep the decorative left-column glow only in the labeled layout.
			bool glow = !icons_only && compact && (i % 2 == 0);
			gridpage_set_tile_icon_style(menu_grid, i, source, icon_size, true,
										 theme_semantic_color(TOKYO_MENU_TONES[i]), glow);
		} else {
			gridpage_set_tile_icon_style(menu_grid, i, source, icon_size, false,
										 theme()->text_primary, false);
		}
		gridpage_set_tile_orientation(menu_grid, i, horizontal);

		lv_obj_set_style_border_width(tile, 0, 0);
		if (tokyo) {
			lv_obj_set_style_outline_width(tile, 0, 0);
		} else {
			lv_obj_remove_local_style_prop(tile, LV_STYLE_OUTLINE_WIDTH, 0);
		}
	}
}

// Fit the V1's live strip around two fixed one-line labels. Explicitly sizing
// the text column from the row's content width keeps both glyphs and ellipses
// inside the card, including after a font-size or theme change.
static void main_menu_layout_tokyo_home(bool force) {
	if (!menu_grid || !menu_cfg || !mini_player || !mini_player_copy ||
			!bp_is_tempotec_v1() || !theme_uses_function_glyphs()) {
		return;
	}

	int line_height = lv_font_get_line_height(&font_ui_14);
	int footer_height = LV_MAX(42, line_height * 2 + 8);
	int footer_y = (int)menu_cfg->screen_height - (int)menu_cfg->padding - footer_height;
	bool needs_reflow = force || (int)lv_obj_get_height(mini_player) != footer_height ||
			(int)lv_obj_get_y(mini_player) != footer_y;
	if (!needs_reflow) {
		return;
	}

	lv_obj_set_size(mini_player, (int)menu_cfg->screen_width - 2 * (int)menu_cfg->padding, footer_height);
	lv_obj_set_pos(mini_player, menu_cfg->padding, footer_y);
	lv_obj_update_layout(mini_player);
	lv_obj_set_height(mini_player_copy, line_height * 2);
	lv_obj_set_flex_grow(mini_player_copy, 0);
	int content_width = lv_obj_get_content_width(mini_player);
	int copy_width = content_width - lv_obj_get_width(mini_player_icon) - 14 - 2 * 7;
	lv_obj_set_width(mini_player_copy, LV_MAX(1, copy_width));
	lv_obj_set_width(mini_player_title, lv_pct(100));
	lv_obj_set_width(mini_player_subtitle, lv_pct(100));
	lv_obj_set_height(mini_player_title, line_height);
	lv_obj_set_height(mini_player_subtitle, line_height);

	int bottom = (int)menu_cfg->padding + footer_height + 1;
	int top = (int)menu_cfg->top_bar_height + (home_header_shown() ? HOME_HEADER_HEIGHT : 0);
	gridpage_set_layout(menu_grid, menu_cfg, top, bottom, menu_cfg->padding, 6, 2, 3);
	main_menu_refresh_tiles();
}

// The mini-player shows the live title and artist only. LVGL owns its label
// text after lv_label_set_text(), so the metadata may safely come from this
// local state snapshot; one-line dot mode clips long text without overlap.
static void main_menu_refresh_now_playing(void) {
	if (!mini_player || !menu_cfg) {
		return;
	}
	main_menu_layout_tokyo_home(false);

	device_state_t state;
	device_state_get(&state);
	bool loaded = state.live || state.current_file[0] != '\0';
	const char *title = NULL;
	const char *artist = "";

	if (!loaded) {
		title = tr("player_no_track_loaded");
		lv_image_set_src(mini_player_icon, &icon_music2);
		lv_obj_set_style_image_recolor(mini_player_icon, theme()->text_secondary, 0);
	} else {
		if (state.metadata.title[0]) {
			title = state.metadata.title;
		} else if (state.live) {
			title = tr("player_live");
		} else {
			const char *file = strrchr(state.current_file, '/');
			title = file ? file + 1 : state.current_file;
		}
		if (state.metadata.artist[0]) {
			artist = state.metadata.artist;
		}

		lv_image_set_src(mini_player_icon,
						 state.status == AUDIO_STATUS_PLAYING ? &icon_pause : &icon_play);
		lv_obj_set_style_image_recolor(mini_player_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
	}

	const lv_image_dsc_t *mini_source = lv_image_get_src(mini_player_icon);
	fit_menu_icon(mini_player_icon, mini_source, 15);
	lv_obj_set_style_image_recolor_opa(mini_player_icon, LV_OPA_COVER, 0);
	if (mini_player_halo) {
		lv_image_set_src(mini_player_halo, mini_source);
		fit_menu_icon(mini_player_halo, mini_source, 21);
		lv_obj_set_style_image_recolor(mini_player_halo, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
		lv_obj_set_style_image_recolor_opa(mini_player_halo, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(mini_player_halo, LV_OPA_20, 0);
		lv_obj_set_hidden(mini_player_halo, !loaded);
		lv_obj_update_layout(mini_player);
		lv_obj_align_to(mini_player_halo, mini_player_icon, LV_ALIGN_CENTER, 0, 0);
	}
	set_label_if_changed(mini_player_title, title ? title : "");
	set_label_if_changed(mini_player_subtitle, artist);
}

static void mini_player_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// This is a 1-second read only while the home screen is actually active.
	// Elsewhere the strip has no pixels to update and the timer returns at once.
	if (lv_screen_active() == main_menu_screen) {
		main_menu_refresh_now_playing();
	}
}

static void main_menu_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	main_menu_refresh_now_playing();
}

static void main_menu_refresh_theme(void) {
	if (!menu_grid || !menu_cfg) {
		return;
	}

	bool compact = bp_is_tempotec_v1();
	bool tokyo = theme_uses_function_glyphs();
	bool prototype_layout = compact && tokyo;

	if (prototype_layout) {
		lv_obj_set_hidden(home_header, !home_header_shown());
		lv_obj_set_hidden(home_note, !home_header_shown());
		lv_obj_set_hidden(mini_player, false);

		// Status bar: y=0..24. The bold 36 px title row and single-line note
		// lead into six horizontal icon-and-label cards; the mini-player sits below.
		// OLED has no title row or note: the cards start under the status bar.
		// The six existing tile objects are simply reflowed, not duplicated.
		// The title rests directly on the Tokyo Night background; the tagline
		// stays below it with a clear gap, and there is no divider.
		lv_obj_set_style_bg_opa(home_header, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(home_header, 0, 0);
		lv_obj_set_style_border_width(home_header, 0, 0);
		lv_obj_set_style_text_font(home_title, &font_ui_20_bold, 0);

		lv_color_t strip_fill = theme_now_playing_fill();
		lv_obj_set_style_bg_color(mini_player, strip_fill, 0);
		lv_obj_set_style_bg_opa(mini_player, LV_OPA_COVER, 0);
		mini_player_clear_edge();
		main_menu_layout_tokyo_home(true);
	} else {
		lv_obj_set_hidden(home_header, true);
		lv_obj_set_hidden(home_note, true);
		lv_obj_set_hidden(mini_player, true);
		gridpage_set_layout(menu_grid, menu_cfg, menu_cfg->top_bar_height, 0, menu_cfg->padding, compact ? 6 : 14, 2, 3);
		main_menu_refresh_tiles();
		lv_obj_set_style_bg_opa(home_header, LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(home_header, 0, 0);
		lv_obj_set_style_text_font(home_title, &font_ui_16, 0);
		lv_obj_set_style_border_width(home_header, 0, 0);
		lv_obj_set_style_border_width(mini_player, 0, 0);
		lv_obj_remove_local_style_prop(mini_player, LV_STYLE_OUTLINE_WIDTH, 0);
		lv_obj_remove_local_style_prop(mini_player, LV_STYLE_BG_COLOR, 0);
	}

	main_menu_refresh_now_playing();
}

void main_menu_set_icons_only(bool enabled) {
	if (main_menu_icons_only() == enabled) {
		return;
	}
	config_set_bool("ui", "main_menu_icons_only", enabled);
	config_save();
	main_menu_refresh_theme();
}

void main_menu_init(gui_config_t *cfg) {
	menu_cfg = cfg;

	const grid_entry_t entries[] = {
		{"music", &icon_menu_music, NULL, open_home_music},
		{"streaming", &icon_menu_streaming, &streaming_screen, NULL},
		{"wireless", &icon_menu_wireless, &wireless_screen, NULL},
		{"audiobooks", &icon_menu_audiobooks, NULL, open_audiobooks},
		// The door to the More page, which holds the DAC and anything else
		// that does not fit. The main menu has exactly six tiles, and one of
		// them has to be able to keep growing.
		{"more", &icon_menu_more, &morepage_screen, NULL},
		{"settings", &icon_menu_settings, &settings_screen, NULL},
	};

	menu_grid = gridpage_build(main_menu_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, false);
	// Cover Flow is a main-menu destination: pull up from the bottom edge,
	// just as the music grid used to, without entering Music first.
	coverflow_attach_edge(menu_grid, cfg);

	// Tokyo Night's compact home chrome: the header, note and live mini-player
	// are ordinary LVGL objects, and remain hidden in the unchanged Dark/Light
	// home menu.
	home_header = lv_obj_create(main_menu_screen);
	lv_obj_remove_style_all(home_header);
	lv_obj_set_size(home_header, cfg->screen_width, 36);
	lv_obj_set_pos(home_header, 0, cfg->top_bar_height);
	lv_obj_set_style_bg_opa(home_header, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(home_header, 0, 0);
	lv_obj_set_style_pad_left(home_header, 6, 0);
	lv_obj_set_style_pad_right(home_header, 6, 0);
	lv_obj_set_style_border_width(home_header, 0, 0);
	lv_obj_set_scrollable(home_header, false);
	lv_obj_set_flex_flow(home_header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	home_title = lv_label_create(home_header);
	lv_label_set_text(home_title, "Morpthis");
	lv_obj_add_style(home_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(home_title, &font_ui_16, 0);
	lv_obj_set_flex_grow(home_title, 1);

	home_note = lv_label_create(main_menu_screen);
	lv_label_set_text(home_note, tr("main_menu_tagline"));
	lv_label_set_long_mode(home_note, LV_LABEL_LONG_DOT);
	lv_obj_set_width(home_note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(home_note, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_pos(home_note, cfg->padding, cfg->top_bar_height + 40);
	lv_obj_add_style(home_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(home_note, &font_ui_14, 0);

	mini_player = lv_btn_create(main_menu_screen);
	lv_obj_set_size(mini_player, cfg->screen_width - 2 * cfg->padding, 38);
	lv_obj_set_pos(mini_player, cfg->padding, cfg->screen_height - cfg->padding - 38);
	lv_obj_add_style(mini_player, &theme_style_card, 0);
	lv_obj_add_style(mini_player, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(mini_player, 8, 0);
	mini_player_clear_edge();
	lv_obj_set_style_pad_left(mini_player, 6, 0);
	lv_obj_set_style_pad_right(mini_player, 6, 0);
	lv_obj_set_style_pad_top(mini_player, 2, 0);
	lv_obj_set_style_pad_bottom(mini_player, 2, 0);
	lv_obj_set_style_pad_gap(mini_player, 7, 0);
	lv_obj_set_scrollable(mini_player, false);
	lv_obj_set_flex_flow(mini_player, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_player, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(mini_player, switch_screen_cb, LV_EVENT_CLICKED, player_screen);
	lv_obj_set_event_bubble(mini_player, true);
	// Compact Amber/Tokyo home menus reserve the bottom band for this footer,
	// so make the same upward pull available here as it is over the card grid.
	coverflow_attach_edge(mini_player, cfg);

	mini_player_halo = lv_image_create(mini_player);
	lv_image_set_src(mini_player_halo, &icon_music2);
	fit_menu_icon(mini_player_halo, &icon_music2, 21);
	lv_obj_set_floating(mini_player_halo, true);
	lv_obj_set_clickable(mini_player_halo, false);
	lv_obj_set_scrollable(mini_player_halo, false);
	lv_obj_set_style_bg_opa(mini_player_halo, LV_OPA_TRANSP, 0);
	lv_obj_set_style_image_opa(mini_player_halo, LV_OPA_20, 0);
	lv_obj_set_hidden(mini_player_halo, true);
	lv_obj_move_to_index(mini_player_halo, 0);

	mini_player_icon = lv_image_create(mini_player);
	lv_image_set_src(mini_player_icon, &icon_music2);
	fit_menu_icon(mini_player_icon, &icon_music2, 15);
	lv_obj_add_style(mini_player_icon, &theme_style_icon, 0);

	mini_player_copy = lv_obj_create(mini_player);
	lv_obj_t *copy = mini_player_copy;
	lv_obj_remove_style_all(copy);
	lv_obj_set_width(copy, LV_SIZE_CONTENT);
	lv_obj_set_height(copy, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(copy, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(copy, 0, 0);
	lv_obj_set_style_pad_gap(copy, 0, 0);
	lv_obj_set_scrollable(copy, false);
	lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_flex_grow(copy, 0);

	mini_player_title = lv_label_create(copy);
	lv_label_set_long_mode(mini_player_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_player_title, lv_pct(100));
	lv_obj_add_style(mini_player_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(mini_player_title, &font_ui_14, 0);

	mini_player_subtitle = lv_label_create(copy);
	lv_label_set_long_mode(mini_player_subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_player_subtitle, lv_pct(100));
	lv_obj_add_style(mini_player_subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mini_player_subtitle, &font_ui_14, 0);

	lv_obj_t *mini_chevron = lv_image_create(mini_player);
	lv_image_set_src(mini_chevron, &icon_chevron_right);
	fit_menu_icon(mini_chevron, &icon_chevron_right, 14);
	lv_obj_add_style(mini_chevron, &theme_style_icon, 0);

	lv_obj_add_event_cb(main_menu_screen, main_menu_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	mini_player_timer = lv_timer_create(mini_player_timer_cb, 1000, NULL);
	if (mini_player_timer) {
		lv_timer_ready(mini_player_timer);
	}

	main_menu_refresh_theme();
	theme_register_refresh(main_menu_refresh_theme);
	fonts_register_change(main_menu_refresh_theme);
}
