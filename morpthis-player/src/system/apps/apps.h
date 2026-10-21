#ifndef APPS_H
#define APPS_H

#include <stdbool.h>
#include <stddef.h>

// Apps on the card.
//
// An app is a folder under <card>/Apps, holding an app.json manifest, an icon
// and an entry binary. The player reads the manifest when the Apps page opens,
// lists what it finds, and starts the entry binary when a row is tapped.
//
// The manifest is the whole contract between the player and an app built in
// another repository:
//
//     {
//       "id": "calculator",
//       "name": "Calculator",
//       "version": "1.0.0",
//       "entry": "calculator",
//       "icon": "icon.png",
//       "description": "A four-function calculator.",
//       "min_player": 1
//     }
//
// `id` defaults to the folder name, `name` to the id, and `icon` to nothing.
// `entry` is required and is relative to the app's own folder. Both `entry`
// and `icon` are refused if they are absolute or contain "..", so a manifest
// cannot point the player outside its own folder.
//
// `tier` is "light" (the default) or "heavy". A light app keeps playback and
// the player's caches as they are. A heavy app asks the player to release its
// playback path and cache memory before the app loads; the queue is kept, so
// pressing play afterwards restarts the current track.
//
// `category` is "app" (the default) or "game". A game gets the whole display:
// no title row, no status bar and no back chevron or edge swipe, and its content
// starts at the top of the panel. A downward swipe from the top edge shows the
// status bar for a second; swiping down again opens the control centre. See
// docs/apps.md.
//
// The launch model is a separate process for headless tools and the SDK's
// in-process model for UI apps. The player tries the plugin first and falls
// back to fork/execve for a tool. See docs/apps.md and docs/app-sdk.md.

#define APPS_DIR "Apps"
#define APPS_MAX 64
#define APPS_ID_MAX 64
#define APPS_NAME_MAX 96
#define APPS_VERSION_MAX 32
#define APPS_ENTRY_MAX 256
#define APPS_ICON_MAX 256
#define APPS_DESC_MAX 160
#define APPS_FOLDER_MAX 512
#define APPS_TIER_MAX 16
#define APPS_CATEGORY_MAX 16

typedef enum {
	APP_TIER_LIGHT = 0,
	APP_TIER_HEAVY,
} app_tier_t;

typedef enum {
	APP_CATEGORY_APP = 0,
	APP_CATEGORY_GAME,
} app_category_t;

typedef struct {
	char id[APPS_ID_MAX];
	char name[APPS_NAME_MAX];
	char version[APPS_VERSION_MAX];
	char entry[APPS_ENTRY_MAX]; // relative to the app's folder
	char icon[APPS_ICON_MAX];	// relative to the app's folder, may be empty
	char description[APPS_DESC_MAX];
	char dir[APPS_ID_MAX];		 // the folder name under Apps/
	char folder[APPS_FOLDER_MAX]; // the absolute path to that folder
	app_tier_t tier;
	app_category_t category;
	int min_player;				 // 0 when the manifest does not say
} app_t;

// Scans <card>/Apps for folders with a readable app.json. Fills up to `max`
// entries, sorted by name, and returns how many were found. Returns -1 when
// there is no card, and 0 when there is no Apps folder yet.
int apps_scan(app_t *out, int max);

// The absolute path of an app's icon, when the manifest names one.
bool apps_icon_path(const app_t *app, char *out, size_t size);

// What happened to a launched app.
typedef struct {
	// False when the child never got as far as execve(): no card, no file, no
	// execute permission, or execve() itself refused. `error` says which.
	bool started;

	// True when the child was killed for outliving the timeout. `signal` is
	// then SIGKILL.
	bool timed_out;

	// The child's exit status, when it exited on its own.
	int exit_code;

	// The signal that ended it, or 0 when it exited.
	int signal;

	// Why it could not be started, when `started` is false. Never empty then.
	char error[128];
} apps_result_t;

// Runs an app's entry binary under the card root, with the app's own folder as
// its working directory, and waits for it.
//
// Blocks the calling thread, so call it from a worker, not the GUI thread.
// The child runs with the player's uid and environment: this is not a security
// boundary.
//
// Returns out->started.
bool apps_launch(const app_t *app, int timeout_ms, apps_result_t *out);

// The primitive behind apps_launch(): runs `relative` (e.g.
// "Apps/calculator/calculator") under the card root. Kept separate so a caller
// with a path rather than a manifest can use the same fork/exec/wait path.
bool apps_run(const char *relative, int timeout_ms, apps_result_t *out);

#endif /* APPS_H */
