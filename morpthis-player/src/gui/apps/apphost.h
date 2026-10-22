#ifndef APPHOST_H
#define APPHOST_H

#include <stdbool.h>
#include <stddef.h>

#include <morpthis/morpthis.h>

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

// Tries the in-process path. The app's package is mounted for the lifetime of
// the app and released by the teardown. `error` is filled when APPHOST_FAILED
// is returned.
apphost_result_t apphost_launch(app_t *app, char *error, size_t error_size);

// The Apps page is on screen. While it is, and while an app or a tool is
// running, USB mass storage stays away from the card: a package is mounted
// from it and the export would unmount the card underneath.
void apphost_set_page_visible(bool visible);
void apphost_set_tool_running(bool running);

// A USB host wants the card while the Apps feature holds it. Called on the GUI
// thread via gui_post(); puts up the confirmation that closes the open app(s),
// leaves the Apps page and lets the watcher export the card.
void apphost_prompt_mass_storage(void *unused);

// True while a confirmed mass-storage handover is closing the app(s). The tool
// result is suppressed in that case; the USB sharing popup is the relevant one.
bool apphost_mass_storage_closing(void);

// True when an app is loaded. Transport keys are the app's while it is: they
// are delivered to its button callback if it set one.
bool apphost_active(void);

// True while the loaded app explicitly holds the device out of standby with
// morpthis_system_hold_awake(). This is the only app-side power hold; raw PCM
// output, background app presence and a busy mixer are not.
bool apphost_awake_hold(void);

// Before entering suspend: a paused app's raw PCM has no reason to keep the
// device awake, and an open PCM makes audio_suspend_freeze() fail forever. The
// SDK hands the output back and reopens the same terms on the next play.
void apphost_suspend_prepare(void);

// After the suspend attempt returns, successful or not. App mixer threads
// parked by apphost_suspend_prepare() wake and reopen their output here.
void apphost_resume_after_suspend(void);

// True when the loaded app asked for the game category: no title row, no status
// bar, no chevron or edge swipe, and the top-edge pull-down is the game's.
bool apphost_game_active(void);

// True when the loaded game has told the player it is in active play rather
// than a menu or pause (morpthis_input_set_gameplay). Only then do the volume
// keys belong to the game instead of the player's volume control.
bool apphost_gameplay_active(void);

// True when the game's own menu is on screen and has asked for the volume
// keys. Menus and save-name entry use this; it does not mean active play.
bool apphost_game_menu_active(void);

// True when the game should receive the volume keys: active play or its menu.
bool apphost_game_input_active(void);

bool apphost_handle_key(gui_key_t key);

// Queues the app's teardown. Idempotent; called by morpthis_app_quit() and by
// the back chevron's guard.
void apphost_request_quit(void);

// Returns to the Apps page without unloading the app. Its screens stay built
// and its threads and audio output keep running, which is what lets a music app
// play in the background.
bool apphost_background(void);

// If `id` is the loaded app (or `id` is NULL for any loaded app), brings its
// active screen back and returns true. Used when its tile is tapped again.
bool apphost_resume_active(const char *id);

// Closes the loaded app and then calls done(user) on the GUI thread, so the
// caller can start the next app. done is called immediately when nothing is
// loaded. Shutdown is bounded: an app that does not return in time is detached
// from its screens and left mapped (force close), and the callback still runs.
void apphost_close_active_then(void (*done)(void *user), void *user);

// The loaded app's raw PCM output, for the status-bar play/pause indicator.
bool apphost_audio_active(void);
bool apphost_audio_playing(void);

// The rate and bit depth of that output, for the status LED's playback colour.
// Zero until the app has opened a stream.
int apphost_audio_rate(void);
int apphost_audio_bits(void);

// The track the loaded app published for the control centre. False when it has
// not published one; `out` is zeroed then.
bool apphost_app_track(morpthis_track_t *out);

// Sends a transport or now-playing request to the loaded app, as if the user
// had pressed its control. False when there is no app to take it.
bool apphost_media_button(morpthis_button_t button);

// Brings the loaded app forward (from the background if needed) and asks it to
// show its now-playing page. False when there is nothing to show.
bool apphost_open_now_playing(void);

// Closes the loaded app's raw PCM output, if it has one. Called when the player
// itself is about to start a local track: one writer on the DAC, and the track
// the user just chose wins over a background app. The app sees a short write
// and stops its own stream.
void apphost_release_audio(void);

#endif /* APPHOST_H */
