#include "apphost.h"

#include <dlfcn.h>
#include <elf.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/apps/appspage.h"
#include "src/gui/apps/sdk_internal.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/switcher.h"
#include "src/system/apps/apps_reclaim.h"
#include "src/system/device/usb.h"

morpthis_app_t *sdk_app;
gui_config_t *sdk_cfg;

static void sync_usb_guard(void);
static void apphost_finish_mass_storage(void);

// Set from the mass-storage confirmation. The card cannot be exported until
// the app and the Apps page have let go, and this is what tells teardown and
// the tool callback to finish the handover once they do.
static bool close_for_mass_storage;

typedef int (*sdk_entry_fn)(const morpthis_app_t *app);
typedef int (*sdk_api_fn)(void);
typedef void (*sdk_shutdown_fn)(void);

static void set_error(char *out, size_t size, const char *format, ...) {
	if (!out || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(out, size, format, args);
	va_end(args);
}

// A shared object is a plugin even when it cannot be loaded: an unresolved SDK
// symbol, a missing dependency. Handing it to execve() next can only produce
// ENOEXEC or SIGILL, which hides the loader's reason. The SDK's tools are
// ordinary executables (ET_EXEC), so the ELF type separates the two cases.
static bool is_shared_object(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}

	unsigned char ident[EI_NIDENT];
	unsigned char type[2];
	bool shared = false;
	if (fread(ident, 1, sizeof(ident), f) == sizeof(ident) &&
		memcmp(ident, ELFMAG, SELFMAG) == 0 &&
		fread(type, 1, sizeof(type), f) == sizeof(type)) {
		uint16_t e_type = ident[EI_DATA] == ELFDATA2MSB ? (uint16_t)((type[0] << 8) | type[1])
																: (uint16_t)((type[1] << 8) | type[0]);
		shared = e_type == ET_DYN;
	}

	fclose(f);
	return shared;
}

static void apphost_teardown(morpthis_app_t *app) {
	if (!app || sdk_app != app) {
		return;
	}

	// Let an app with threads or the raw audio output stop its own work while
	// its context and screen still exist. Optional: an app that never defined
	// the symbol has nothing to do. quitting is already set for a normal quit;
	// set it here too so a shutdown that calls morpthis_app_quit() cannot post
	// a second teardown from inside this one.
	app->quitting = true;
	if (app->shutdown) {
		app->shutdown();
		app->shutdown = NULL;
	}

	app->alive = false;

	// Leave the app's screen before deleting it: LVGL must never be sitting on
	// an object that is about to go away. When a PC is taking the card, go to
	// More and not back to Apps: that page mounts packages, and the card is
	// about to be handed to the host.
	if (app->active) {
		switch_screen_return_to(close_for_mass_storage ? morepage_screen : appspage_screen);
		app->active = NULL;
	} else if (close_for_mass_storage) {
		switch_screen(morepage_screen);
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

	// The entry binary and its assets came from the package mount; nothing may
	// read them now that the shared object is closed.
	apps_unmount(app->folder);

	// The Apps page is normally back on screen by now. Recompute anyway, so a
	// teardown reached from somewhere else cannot leave the USB guard up.
	if (close_for_mass_storage) {
		apphost_finish_mass_storage();
	} else {
		sync_usb_guard();
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

// ---------------------------------------------------------------------------
// The USB guard
//
// An app's package is a squashfs image on the card, and USB mass storage
// stops the card being mounted at all: exporting it unmounts the card and
// hands the raw device to the PC. That is fine on the music pages and fatal
// here. The page, a loaded plugin and a running tool each hold a reason, so
// the guard comes off only when the Apps feature is entirely away.
// ---------------------------------------------------------------------------

static bool apps_page_visible;
static int tools_running;

static void sync_usb_guard(void) {
	usb_set_apps_mode(apps_page_visible || apphost_active() || tools_running > 0);
}

// Leaves the Apps page when the card is being handed over: the page mounts
// packages, and a scan landing on an exported card is exactly what the guard
// exists to prevent.
static void apphost_finish_mass_storage(void) {
	if (!close_for_mass_storage) {
		return;
	}
	close_for_mass_storage = false;
	if (apps_page_visible) {
		switch_screen(morepage_screen);
	}
	sync_usb_guard();
}

static void mass_storage_confirmed(void *user) {
	(void)user;
	close_for_mass_storage = true;

	if (sdk_app && sdk_app->alive) {
		// The normal quit path: the app's optional shutdown hook runs before
		// its screens and the shared object go away.
		apphost_request_quit();
		return;
	}

	if (tools_running > 0) {
		// The worker's completion callback finishes the handover.
		apps_stop_running_tool();
		return;
	}

	apphost_finish_mass_storage();
}

void apphost_prompt_mass_storage(void *unused) {
	(void)unused;
	// A PC wants the card while the Apps feature holds it. The dialog is safe
	// over a game or any app screen: confirm.c draws on LVGL's top layer.
	confirm_show("usb_storage_close_apps_title", "usb_storage_close_apps_note", "usb_storage_use",
				 mass_storage_confirmed, NULL);
}

bool apphost_mass_storage_closing(void) {
	return close_for_mass_storage;
}

void apphost_set_page_visible(bool visible) {
	apps_page_visible = visible;
	sync_usb_guard();
}

void apphost_set_tool_running(bool running) {
	if (running) {
		tools_running++;
	} else if (tools_running > 0) {
		tools_running--;
	}
	sync_usb_guard();

	// The last tool stopped because the card is being handed to a PC. Nothing
	// else holds the guard, so finish the handover now.
	if (!running && close_for_mass_storage && tools_running == 0 && !apphost_active()) {
		apphost_finish_mass_storage();
	}
}

void apphost_init(gui_config_t *cfg) {
	sdk_cfg = cfg;
	sdk_app = NULL;
	apps_page_visible = false;
	tools_running = 0;
}

bool apphost_active(void) {
	return sdk_app && sdk_app->alive;
}

bool apphost_game_active(void) {
	return sdk_app && sdk_app->alive && sdk_app->category == APP_CATEGORY_GAME;
}

bool apphost_gameplay_active(void) {
	return apphost_game_active() && sdk_app->gameplay;
}

bool apphost_game_menu_active(void) {
	return apphost_game_active() && sdk_app->game_menu;
}

bool apphost_game_input_active(void) {
	return apphost_game_active() && (sdk_app->gameplay || sdk_app->game_menu);
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
	case GUI_KEY_VOLUME_UP:
		button = MORPTHIS_BUTTON_VOLUME_UP;
		break;
	case GUI_KEY_VOLUME_DOWN:
		button = MORPTHIS_BUTTON_VOLUME_DOWN;
		break;
	default:
		return false;
	}

	// The physical keys arrive as a press; this player has no release event for
	// them. The API allows an app to see PRESS only.
	sdk_dispatch_button(button, MORPTHIS_INPUT_PRESS);
	return true;
}

apphost_result_t apphost_launch(app_t *meta, char *error, size_t error_size) {
	if (!meta || !meta->entry[0] || !meta->dir[0]) {
		set_error(error, error_size, "no app");
		return APPHOST_FAILED;
	}
	if (sdk_app) {
		set_error(error, error_size, "another app is running");
		return APPHOST_FAILED;
	}

	// The entry and everything beside it live in a read-only image. Keep the
	// mount up for as long as the shared object can run code from it.
	char why[128];
	if (!apps_mount(meta, why, sizeof(why))) {
		set_error(error, error_size, "%s", why);
		fprintf(stderr, "apps: %s: %s\n", meta->package, why);
		return APPHOST_FAILED;
	}

	char path[APPS_FOLDER_MAX + APPS_ENTRY_MAX + 8];
	int n = snprintf(path, sizeof(path), "%s/%s", meta->folder, meta->entry);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		apps_unmount(meta->folder);
		set_error(error, error_size, "path too long");
		return APPHOST_FAILED;
	}

	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char *loader_error = dlerror();
		bool shared = is_shared_object(path);
		apps_unmount(meta->folder);
		if (shared) {
			set_error(error, error_size, "%s", loader_error ? loader_error : "cannot load the plugin");
			fprintf(stderr, "apps: %s is a plugin that cannot load: %s\n", path,
					loader_error ? loader_error : "no dlopen error");
			return APPHOST_FAILED;
		}
		// Not a plugin is the normal case for a headless tool. The apps page
		// takes the fork/execve path next; the log line is what explains why a
		// UI app took it too.
		fprintf(stderr, "apps: %s is not a plugin (%s); trying it as a tool\n", path,
				loader_error ? loader_error : "no dlopen error");
		return APPHOST_NOT_PLUGIN;
	}

	sdk_entry_fn entry = (sdk_entry_fn)dlsym(handle, "morpthis_app_main");
	sdk_api_fn required = (sdk_api_fn)dlsym(handle, "morpthis_app_api_required");
	sdk_shutdown_fn shutdown = (sdk_shutdown_fn)dlsym(handle, "morpthis_app_shutdown");
	if (!entry) {
		bool shared = is_shared_object(path);
		dlclose(handle);
		apps_unmount(meta->folder);
		if (shared) {
			set_error(error, error_size, "no morpthis_app_main");
			fprintf(stderr, "apps: %s is a plugin without morpthis_app_main\n", path);
			return APPHOST_FAILED;
		}
		fprintf(stderr, "apps: %s has no morpthis_app_main; trying it as a tool\n", path);
		return APPHOST_NOT_PLUGIN;
	}

	int api = required ? required() : 1;
	if (api > MORPTHIS_APP_API_VERSION) {
		set_error(error, error_size, "needs a newer player (app API %d, player API %d)", api,
				  MORPTHIS_APP_API_VERSION);
		dlclose(handle);
		apps_unmount(meta->folder);
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
		apps_unmount(meta->folder);
		return APPHOST_FAILED;
	}

	snprintf(app->id, sizeof(app->id), "%s", meta->id);
	snprintf(app->name, sizeof(app->name), "%s", meta->name);
	snprintf(app->version, sizeof(app->version), "%s", meta->version);
	snprintf(app->folder, sizeof(app->folder), "%s", meta->folder);
	snprintf(app->data, sizeof(app->data), "%s", meta->data);
	app->tier = meta->tier;
	app->category = meta->category;
	app->dl_handle = handle;
	app->shutdown = shutdown;
	app->alive = true;
	sdk_set_app(app);

	// The page will unload as the app's screen goes up; keep the card out of
	// USB mass storage before that transition, so the unload cannot be the
	// first thing to notice.
	sync_usb_guard();

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
