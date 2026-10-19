#include "streaming.h"

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/subsonicpage.h"

lv_obj_t *streaming_screen;

static lv_obj_t *streaming_grid;
static lv_obj_t *streaming_list;

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;
	theme_semantic_t tone;
	lv_obj_t **target;
} streaming_item_t;

static const streaming_item_t tokyo_items[] = {
	{"radio", &icon_radio_player, THEME_SEMANTIC_PURPLE, &radiopage_screen},
	{"podcasts", &icon_podcast_list, THEME_SEMANTIC_CYAN, &podcast_screen},
	{"tidal", &icon_tidal_mark, THEME_SEMANTIC_CYAN, &tidal_screen},
	{"qobuz", &icon_qobuz_mark, THEME_SEMANTIC_PURPLE, &qobuz_screen},
	{"subsonic", &icon_music_note, THEME_SEMANTIC_BLUE, &subsonic_screen},
};

static void streaming_list_build(gui_config_t *cfg) {
	streaming_list = settingsrow_page(streaming_screen, cfg, "streaming");
	player_sheet_attach_drag(streaming_list, true);

	for (unsigned int i = 0; i < sizeof(tokyo_items) / sizeof(tokyo_items[0]); i++) {
		const streaming_item_t *item = &tokyo_items[i];
		lv_obj_t *row = settingsrow_add(streaming_list, item->label, NULL, switch_screen_cb, *item->target);
		settingsrow_add_icon(row, item->icon, item->tone);
	}
}

static void streaming_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_uses_function_glyphs();
	if (streaming_grid) {
		lv_obj_set_hidden(streaming_grid, tokyo_v1);
	}
	if (streaming_list) {
		lv_obj_set_hidden(streaming_list, !tokyo_v1);
	}
}

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);
	subsonicpage_init(cfg);

	const grid_entry_t entries[] = {
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
		{"subsonic", &icon_menu_streaming, &subsonic_screen, NULL},
	};

	// Dark and Light keep their established two-column grid. Tokyo Night on the
	// V1 gets the same destinations as a compact, icon-led list.
	streaming_grid = gridpage_build(streaming_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3,
									 true);
	if (bp_is_tempotec_v1()) {
		streaming_list_build(cfg);
	} else {
		settingsrow_title(streaming_screen, cfg, "streaming");
	}
	streaming_refresh_theme();
	theme_register_refresh(streaming_refresh_theme);
}
