#ifndef APPHOST_H
#define APPHOST_H

#include <stddef.h>

#include "src/gui/shell/gui.h"
#include "src/system/apps/apps.h"

// The in-process app host.
//
// A UI app is a shared object. dlopen() is tried first; when the entry names an
// ordinary executable (a headless tool) the apps page keeps its fork/execve
// path. The host calls the entry on the GUI thread and keeps the app's screens
// and SDK state until it quits.

typedef enum {
	APPHOST_NOT_PLUGIN, // not a shared object with an entry; use fork/exec
	APPHOST_STARTED,	// the app is running (or its quit is already queued)
	APPHOST_FAILED,		// it is a plugin, but it could not start
} apphost_result_t;

// Called once from gui_init(), after the theme exists.
void apphost_init(gui_config_t *cfg);

// Tries the in-process path. `error` is filled when APPHOST_FAILED is returned.
apphost_result_t apphost_launch(const app_t *app, char *error, size_t error_size);

// True when an app is loaded. Transport keys are the app's while it is: they
// are delivered to its button callback if it set one.
bool apphost_active(void);

// True when the loaded app asked for the game category: no title row, no status
// bar, no chevron or edge swipe, and the top-edge pull-down is the game's.
bool apphost_game_active(void);

// True when the loaded game has told the player it is in active play rather
// than a menu or pause (morpthis_input_set_gameplay). Only then do the volume
// keys belong to the game instead of the player's volume control.
bool apphost_gameplay_active(void);

bool apphost_handle_key(gui_key_t key);

// Queues the app's teardown. Idempotent; called by morpthis_app_quit() and by
// the back chevron's guard.
void apphost_request_quit(void);

#endif /* APPHOST_H */
