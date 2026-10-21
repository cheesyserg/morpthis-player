#include "apphost.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/apps/appspage.h"
#include "src/gui/apps/sdk_internal.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/system/apps/apps_reclaim.h"

morpthis_app_t *sdk_app;
gui_config_t *sdk_cfg;

typedef int (*sdk_entry_fn)(const morpthis_app_t *app);
typedef int (*sdk_api_fn)(void);

static void set_error(char *out, size_t size, const char *format, ...) {
	if (!out || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(out, size, format, args);
	va_end(args);
}

static void apphost_teardown(morpthis_app_t *app) {
	if (!app || sdk_app != app) {
		return;
	}

	app->alive = false;

	// Leave the app's screen before deleting it: LVGL must never be sitting on
	// an object that is about to go away.
	if (app->active) {
		switch_screen_return_to(appspage_screen);
		app->active = NULL;
	}

	while (app->screens) {
		sdk_screen_destroy(app->screens);
	}

	// Nothing may enter the shared object after this point. Posts already
	// waiting in the GUI queue are detached; a callback that arrives after
	// this sees no app and drops itself. The app is required to have stopped
	// its own threads before quitting.
	sdk_detach_app(app);

	if (app->dl_handle) {
		dlclose(app->dl_handle);
		app->dl_handle = NULL;
	}

	// The heavy tier dropped the player's artwork to make room; ask for the
	// current track's picture again now that the app is gone.
	if (app->tier == APP_TIER_HEAVY) {
		player_refresh_artwork();
	}

	fprintf(stderr, "apps: %s unloaded\n", app->id);
	free(app);
}

static void teardown_cb(void *user) {
	apphost_teardown((morpthis_app_t *)user);
}

void apphost_request_quit(void) {
	morpthis_app_t *app = sdk_app;
	if (!app || !app->alive || app->quitting) {
		return;
	}
	app->quitting = true;
	if (!gui_post(teardown_cb, app)) {
		// The GUI queue is full. There is no safe second route that does not
		// delete LVGL objects from inside a callback; say so and let the next
		// quit request try again.
		app->quitting = false;
		fprintf(stderr, "apps: could not queue teardown for %s\n", app->id);
	}
}

void apphost_init(gui_config_t *cfg) {
	sdk_cfg = cfg;
	sdk_app = NULL;
}

bool apphost_active(void) {
	return sdk_app && sdk_app->alive;
}

bool apphost_handle_key(gui_key_t key) {
	morpthis_app_t *app = sdk_app;
	if (!app || !app->alive || !app->button_cb) {
		return false;
	}

	morpthis_button_t button;
	switch (key) {
	case GUI_KEY_PLAY_PAUSE:
		button = MORPTHIS_BUTTON_PLAY_PAUSE;
		break;
	case GUI_KEY_NEXT:
		button = MORPTHIS_BUTTON_NEXT;
		break;
	case GUI_KEY_PREV:
		button = MORPTHIS_BUTTON_PREV;
		break;
	default:
		return false;
	}

	// The physical keys arrive as a press; this player has no release event for
	// them. The API allows an app to see PRESS only.
	sdk_dispatch_button(button, MORPTHIS_INPUT_PRESS);
	return true;
}

apphost_result_t apphost_launch(const app_t *meta, char *error, size_t error_size) {
	if (!meta || !meta->folder[0] || !meta->entry[0]) {
		set_error(error, error_size, "no app");
		return APPHOST_FAILED;
	}
	if (sdk_app) {
		set_error(error, error_size, "another app is running");
		return APPHOST_FAILED;
	}

	char path[APPS_FOLDER_MAX + APPS_ENTRY_MAX + 8];
	int n = snprintf(path, sizeof(path), "%s/%s", meta->folder, meta->entry);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		set_error(error, error_size, "path too long");
		return APPHOST_FAILED;
	}

	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char *why = dlerror();
		// Not a plugin is the normal case for a headless tool. The apps page
		// takes the fork/execve path next; the log line is what explains why a
		// UI app took it too.
		fprintf(stderr, "apps: %s is not a plugin (%s); trying it as a tool\n", path,
				why ? why : "no dlopen error");
		return APPHOST_NOT_PLUGIN;
	}

	sdk_entry_fn entry = (sdk_entry_fn)dlsym(handle, "morpthis_app_main");
	sdk_api_fn required = (sdk_api_fn)dlsym(handle, "morpthis_app_api_required");
	if (!entry) {
		fprintf(stderr, "apps: %s has no morpthis_app_main; trying it as a tool\n", path);
		dlclose(handle);
		return APPHOST_NOT_PLUGIN;
	}

	int api = required ? required() : 1;
	if (api > MORPTHIS_APP_API_VERSION) {
		set_error(error, error_size, "needs a newer player (app API %d, player API %d)", api,
				  MORPTHIS_APP_API_VERSION);
		dlclose(handle);
		return APPHOST_FAILED;
	}

	// The heavy tier's one big release happens once the file is known to be a
	// plugin, before its entry point runs. A tool keeps the light path.
	if (meta->tier == APP_TIER_HEAVY) {
		apps_reclaim_for_heavy(meta);
	}

	morpthis_app_t *app = calloc(1, sizeof(*app));
	if (!app) {
		set_error(error, error_size, "out of memory");
		dlclose(handle);
		return APPHOST_FAILED;
	}

	snprintf(app->id, sizeof(app->id), "%s", meta->id);
	snprintf(app->name, sizeof(app->name), "%s", meta->name);
	snprintf(app->version, sizeof(app->version), "%s", meta->version);
	snprintf(app->folder, sizeof(app->folder), "%s", meta->folder);
	app->tier = meta->tier;
	app->dl_handle = handle;
	app->alive = true;
	sdk_set_app(app);

	fprintf(stderr, "apps: %s %s (API %d, %s tier) loading in-process\n", app->name, app->version, api,
			app->tier == APP_TIER_HEAVY ? "heavy" : "light");

	int rc = entry(app);
	if (rc != 0) {
		if (app->quitting) {
			// The entry asked to quit and then returned non-zero. The queued
			// teardown is the one that owns the context.
			return APPHOST_STARTED;
		}
		set_error(error, error_size, "entry returned %d", rc);
		apphost_teardown(app);
		return APPHOST_FAILED;
	}

	if (!app->active && !app->quitting) {
		set_error(error, error_size, "did not load a screen");
		apphost_teardown(app);
		return APPHOST_FAILED;
	}

	return APPHOST_STARTED;
}
