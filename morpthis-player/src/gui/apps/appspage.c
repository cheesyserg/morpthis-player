#include "appspage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/apps/apphost.h"
#include "src/system/apps/apps.h"
#include "src/system/core/lang.h"

lv_obj_t *appspage_screen;

static gui_config_t *page_cfg;
static lv_obj_t *grid;
static lv_obj_t *empty_panel;
static lv_obj_t *loading_label;

static app_t apps[APPS_MAX];
static int app_count;

// The tiles are rebuilt on every open, but the entries have to live for the
// whole run: a tile keeps a pointer to its entry (for the launch callback) and
// the grid keeps the icon descriptors across a theme change.
static grid_entry_t entries[APPS_MAX];
static cover_image_t icons[APPS_MAX];

// A UI app is a shared object the SDK host loads into this process. A headless
// tool is still a separate process: the file is not a shared object (or has no
// SDK entry), so the page falls back to fork/execve and waits for it.
#define APPS_LAUNCH_TIMEOUT_MS 30000

typedef struct {
	app_t app;
} launch_job_t;

typedef struct {
	char name[APPS_NAME_MAX];
	apps_result_t result;
} launch_result_t;

static void rebuild(void);

static void launch_done_cb(void *user) {
	launch_result_t *done = user;
	char text[APPS_NAME_MAX + 96];

	// The card is free again for whichever reason the tool ended. The guard is
	// dropped before the result is shown; if the page is still up it stays on
	// because the page itself holds it. Capture the handover first: dropping
	// the tool guard may finish it and clear the flag.
	bool closing = apphost_mass_storage_closing();
	apphost_set_tool_running(false);
	if (closing) {
		// The tool was stopped because the card is going to the host. The USB
		// popup is the result the user needs, not "signal 15".
		free(done);
		return;
	}

	if (!done->result.started) {
		snprintf(text, sizeof(text), "%s: %s", done->name, done->result.error);
		toast_error(text);
	} else if (done->result.timed_out) {
		snprintf(text, sizeof(text), "%s: timed out", done->name);
		toast_error(text);
	} else if (done->result.signal) {
		snprintf(text, sizeof(text), "%s: signal %d", done->name, done->result.signal);
		toast_error(text);
	} else if (done->result.exit_code != 0) {
		snprintf(text, sizeof(text), "%s: exit %d", done->name, done->result.exit_code);
		toast_error(text);
	} else {
		snprintf(text, sizeof(text), "%s: exit 0", done->name);
		toast_success(text);
	}

	free(done);
}

static void *launch_thread(void *arg) {
	launch_job_t *job = arg;

	launch_result_t *done = calloc(1, sizeof(*done));
	if (done) {
		snprintf(done->name, sizeof(done->name), "%s", job->app.name);
		apps_launch(&job->app, APPS_LAUNCH_TIMEOUT_MS, &done->result);
		if (!gui_post(launch_done_cb, done)) {
			free(done);
		}
	}

	free(job);
	return NULL;
}

static void launch_app(app_t *app) {
	// The in-process path first. It runs on the GUI thread because the entry
	// point builds screens; a plugin that cannot start is reported here rather
	// than run as a tool, which would only produce ENOEXEC.
	char error[128];
	apphost_result_t result = apphost_launch(app, error, sizeof(error));
	if (result == APPHOST_STARTED) {
		return;
	}
	if (result == APPHOST_FAILED) {
		char text[APPS_NAME_MAX + 160];
		snprintf(text, sizeof(text), "%s: %s", app->name, error);
		toast_error(text);
		return;
	}

	// APPHOST_NOT_PLUGIN: a headless tool. The existing worker keeps the GUI
	// responsive while fork/execve runs and waits. The app is copied out of the
	// page's array: the page may be rebuilt while the worker is still starting
	// it, and the tile would then name a different app.
	launch_job_t *job = malloc(sizeof(*job));
	if (!job) {
		return;
	}
	job->app = *app;

	// The tool runs from the package mount, so keep USB mass storage off the
	// card until the worker has finished with it. The worker posts the result
	// back to the GUI thread, and launch_done_cb() drops the guard there.
	apphost_set_tool_running(true);

	pthread_t thread;
	if (pthread_create(&thread, NULL, launch_thread, job) == 0) {
		pthread_detach(thread);
	} else {
		apphost_set_tool_running(false);
		free(job);
	}
}

static void launch_tile_cb(void *user) { launch_app((app_t *)user); }

static void free_icons(void) {
	for (int i = 0; i < APPS_MAX; i++) {
		free(icons[i].pixels);
		memset(&icons[i], 0, sizeof(icons[i]));
	}
}

// The apps page uses the player's tiled menu, like the main menu and More:
// one card per app, the app's own icon when it has one, the name underneath.
static void rebuild(void) {
	// Hide the old tiles before their icons are freed; nothing gets a chance to
	// draw a descriptor that is about to go away. The empty panel stays hidden
	// too: while the scan runs the loading line is the only thing on the page.
	lv_obj_set_hidden(grid, true);
	lv_obj_set_hidden(empty_panel, true);
	free_icons();

	app_count = apps_scan(apps, APPS_MAX);
	if (app_count <= 0) {
		gridpage_empty_set_text(empty_panel, app_count < 0 ? "system_no_card" : "apps_empty_note");
		gridpage_show_empty(grid, empty_panel, true);
		return;
	}

	// The icon is decoded to the size it is drawn at. The V1's tile icons are
	// 46 px; the regular panel uses the same 112 px as the other section tiles.
	int box = page_cfg && page_cfg->screen_width < 320 ? 46 : 112;
	char path[APPS_FOLDER_MAX + APPS_ICON_MAX + 2];

	for (int i = 0; i < app_count; i++) {
		const lv_image_dsc_t *icon = &icon_menu_more;
		if (apps_icon_path(&apps[i], path, sizeof(path)) &&
			cover_load_image_file(path, box, box, COVER_FIT_CONTAIN, &icons[i])) {
			icon = &icons[i].dsc;
		}
		entries[i] = (grid_entry_t){
			.label = apps[i].name,
			.icon = icon,
			.action_user = launch_tile_cb,
			.user = &apps[i],
			// An app's icon is its own colour drawing; the monochrome layout
			// must not recolor it or swap in one of its menu glyphs.
			.keep_color = true,
		};
	}

	gridpage_replace_entries(grid, page_cfg, entries, app_count, 2, 3);
	gridpage_show_empty(grid, empty_panel, false);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card must be back before the scan mounts a package from it. This is
	// also what turns a card already exported to a PC into the player's again.
	apphost_set_page_visible(true);

	// The scan mounts every package and decodes every icon, which blocks the
	// interface for a moment. Paint the loading line first, or the user sees
	// the previous page frozen while it happens.
	if (loading_label) {
		lv_obj_set_hidden(loading_label, false);
		lv_obj_move_foreground(loading_label);
		lv_obj_set_hidden(grid, true);
		lv_obj_set_hidden(empty_panel, true);
		lv_refr_now(NULL);
	}
	rebuild();
	if (loading_label) {
		lv_obj_set_hidden(loading_label, true);
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// An app launch unloads this page too; the app itself holds the guard, so
	// this only releases the page's reason.
	apphost_set_page_visible(false);
}

static void scan_cb(lv_event_t *e) {
	(void)e;
	if (loading_label) {
		lv_obj_set_hidden(loading_label, false);
		lv_obj_move_foreground(loading_label);
		lv_obj_set_hidden(empty_panel, true);
		lv_refr_now(NULL);
	}
	rebuild();
	if (loading_label) {
		lv_obj_set_hidden(loading_label, true);
	}
}

static void info_cb(lv_event_t *e) {
	(void)e;
	// The page's one piece of running text. It lives behind the corner button
	// instead of above the tiles, which is what the stock menus do with prose.
	confirm_notice("apps", "apps_note");
}

// The same corner button every other page uses: a bare glyph in the top right,
// in the theme's text colour.
static void corner_button(gui_config_t *cfg, const lv_image_dsc_t *glyph, lv_event_cb_t cb, void *user) {
	lv_obj_t *button = lv_btn_create(appspage_screen);
	settingsrow_place_corner_button(button, cfg, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
}

void appspage_init(gui_config_t *cfg) {
	page_cfg = cfg;

	settingsrow_title(appspage_screen, cfg, "apps");
	corner_button(cfg, &icon_info, info_cb, NULL);

	// Built empty and filled by rebuild() on every open: the card is the source
	// of truth, and it can change between visits. Clear the corner-button band
	// so no tile sits under the info button.
	grid = gridpage_build(appspage_screen, cfg, entries, 0, 2, 3, true);
	empty_panel = gridpage_empty_panel(appspage_screen, cfg, &icon_menu_more, "apps_empty_note", scan_cb);

	// Said before the first scan, over the empty grid. The page is rebuilt on
	// every open, so this is the line the user sees while it mounts packages.
	loading_label = lv_label_create(appspage_screen);
	lv_label_set_text(loading_label, tr("loading"));
	lv_obj_add_style(loading_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(loading_label, &font_ui_22, 0);
	lv_obj_center(loading_label);
	lv_obj_set_hidden(loading_label, true);

	lv_obj_add_event_cb(appspage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(appspage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
