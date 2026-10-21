#ifndef APPS_H
#define APPS_H

#include <stdbool.h>
#include <stddef.h>

// Apps on the card.
//
// An app is one .pkgmerp file under <card>/Apps/pkgmerps: an uncompressed
// squashfs image holding app.json, an icon and the entry binary at its root.
// The player mounts the image read-only through a loop device when the Apps
// page opens, reads the manifest, unmounts it, and mounts it again when a row
// is tapped. Nothing is extracted to the card, and replacing the file is the
// whole update.
//
// An app's writable state lives outside the image, in <card>/merp_data/<id>.
// The package is read-only by construction, so the SDK's fs calls read from the
// data folder first and fall back to the package, and every write goes to the
// data folder. Deleting the package leaves the data behind; deleting both
// uninstalls the app.
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
// `id` defaults to the package name, `name` to the id, and `icon` to nothing.
// `entry` is required and is relative to the package root. Both `entry` and
// `icon` are refused if they are absolute or contain "..", so a manifest
// cannot point the player outside its own image.
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
#define APPS_PACKAGE_DIR "pkgmerps"
#define APPS_PACKAGE_EXT ".pkgmerp"
#define APPS_DATA_DIR "merp_data"
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
	char entry[APPS_ENTRY_MAX]; // relative to the package root
	char icon[APPS_ICON_MAX];	// relative to the package root, may be empty
	char description[APPS_DESC_MAX];
	char dir[APPS_ID_MAX];		 // the package name, without .pkgmerp
	char package[APPS_FOLDER_MAX]; // the absolute .pkgmerp file
	char folder[APPS_FOLDER_MAX];  // the mount point, valid while mounted
	char data[APPS_FOLDER_MAX];	 // the absolute merp_data folder
	bool icon_cached;			 // icon[] is an absolute cached path
	app_tier_t tier;
	app_category_t category;
	int min_player;				 // 0 when the manifest does not say
} app_t;

// Scans <card>/Apps/pkgmerps for .pkgmerp images. Each one is mounted just long
// enough to read app.json and copy the icon to a cache, then unmounted. Fills
// up to `max` entries, sorted by name, and returns how many were found.
// Returns -1 when there is no card, and 0 when there is no pkgmerps folder.
int apps_scan(app_t *out, int max);

// Mounts an app's package read-only and creates its data folder. Fills
// `folder` and `data`. The mount must be released with apps_unmount() before
// the package file can be replaced. Returns false and sets `error` on failure.
bool apps_mount(app_t *app, char *error, size_t error_size);

// Unmounts the package mounted on `mount_point` and releases its loop device.
// A path this process did not mount is ignored.
void apps_unmount(const char *mount_point);

// The absolute path of an app's icon, when the manifest names one. The scan
// copies it out of the package, so the path stays valid after the unmount.
bool apps_icon_path(const app_t *app, char *out, size_t size);

// What happened to a launched app.
typedef struct {
	// False when the child never got as far as execve(): the package could not
	// be mounted, the entry is missing, execute permission is missing, or
	// execve() itself refused. `error` says which.
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

// Mounts an app's package, runs its entry binary with the package root as its
// working directory, waits, and unmounts it.
//
// Blocks the calling thread, so call it from a worker, not the GUI thread.
// The child also receives MORPTHIS_APP_FOLDER and MORPTHIS_APP_DATA in its
// environment, so a tool that writes knows where the writable folder is. It
// runs with the player's uid and environment otherwise: this is not a security
// boundary.
//
// Returns out->started.
bool apps_launch(app_t *app, int timeout_ms, apps_result_t *out);

// Gracefully stops a tool child started by apps_launch(): SIGTERM to the
// child's process group, then to the child itself. The worker still owns the
// wait and reaps it; a child that ignores the signal is killed by the launch
// timeout. Safe to call when no tool is running.
void apps_stop_running_tool(void);

// The primitive behind apps_launch(): runs the absolute `path`, with
// `folder` as the working directory, and passes `data` to the child as
// MORPTHIS_APP_DATA. Kept separate so a caller with a path rather than a
// manifest can use the same fork/exec/wait path.
bool apps_run(const char *path, const char *folder, const char *data,
			  int timeout_ms, apps_result_t *out);

#endif /* APPS_H */
