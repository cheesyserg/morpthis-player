#include "morepage.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/board_profile.h"
#include "src/gui/library/filespage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

lv_obj_t *morepage_screen;

static lv_obj_t *more_grid;
static lv_obj_t *more_list;

static void more_action_cb(lv_event_t *e) {
	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	if (action) {
		action();
	}
}

static bool more_tokyo_icon(const char *label, const lv_image_dsc_t **icon, theme_semantic_t *tone) {
	if (!label || !icon || !tone) {
		return false;
	}
	if (strcmp(label, "dac") == 0) {
		*icon = &icon_usbaudioout;
		*tone = THEME_SEMANTIC_CYAN;
	} else if (strcmp(label, "file_explorer") == 0) {
		*icon = &icon_folder;
		*tone = THEME_SEMANTIC_BLUE;
	} else {
		return false;
	}
	return true;
}

static void more_list_add_entry(const grid_entry_t *entry) {
	lv_event_cb_t callback = entry->target ? switch_screen_cb : more_action_cb;
	void *user_data = entry->target ? (void *)*entry->target : (void *)entry->action;
	lv_obj_t *row = settingsrow_add(more_list, entry->label, NULL, callback, user_data);
	const lv_image_dsc_t *icon = NULL;
	theme_semantic_t tone;
	if (more_tokyo_icon(entry->label, &icon, &tone)) {
		settingsrow_add_icon(row, icon, tone);
	}
}

static void more_list_build(gui_config_t *cfg, const grid_entry_t *entries, int count) {
	static const char *const priority_labels[] = {"dac", "file_explorer"};
	more_list = settingsrow_page(morepage_screen, cfg, "more");
	player_sheet_attach_drag(more_list, true);

	// Keep the File Manager near the top on the short V1 screen, where the
	// bottom rows otherwise fall below the initial viewport.
	for (size_t p = 0; p < sizeof(priority_labels) / sizeof(priority_labels[0]); p++) {
		for (int i = 0; i < count; i++) {
			if (strcmp(entries[i].label, priority_labels[p]) == 0) {
				more_list_add_entry(&entries[i]);
				break;
			}
		}
	}

	for (int i = 0; i < count; i++) {
		if (strcmp(entries[i].label, "dac") == 0 || strcmp(entries[i].label, "file_explorer") == 0) {
			continue;
		}
		more_list_add_entry(&entries[i]);
	}
}

// The shape of the tiled page. More holds two destinations; keep the
// three-row tile size other sections use rather than stretching them.
#define MORE_GRID_ROWS 3

static void more_refresh_theme(void) {
	bool tokyo_v1 = bp_is_tempotec_v1() && theme_uses_function_glyphs();
	if (more_grid) {
		lv_obj_set_hidden(more_grid, tokyo_v1);
	}
	if (more_list) {
		lv_obj_set_hidden(more_list, !tokyo_v1);
	}
}

void morepage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
		// An action rather than a screen: the listing is read when the page is
		// entered, so a card that changed under the player shows what is on it
		// now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
	};

	// Dark and Light retain the established two-column grid. Tokyo Night uses a
	// compact, single-column list whose rows share the Settings list styling.
	int count = (int)(sizeof(entries) / sizeof(entries[0]));
	more_grid = gridpage_build(morepage_screen, cfg, entries, count, 2, MORE_GRID_ROWS, true);
	if (bp_is_tempotec_v1()) {
		more_list_build(cfg, entries, count);
	} else {
		settingsrow_title(morepage_screen, cfg, "more");
	}
	more_refresh_theme();
	theme_register_refresh(more_refresh_theme);
}
