#ifndef PLAYER_H
#define PLAYER_H

#include "src/gui/shell/gui.h"
#include "src/system/library/library.h"
#include "src/system/streaming/podcast.h" // podcast_feed_t

#include "lvgl/lvgl.h"

extern lv_obj_t *player_screen;

void player_init(gui_config_t *cfg);

// Start playing the given file and switch its now-playing info in the UI.
// Does not bring the player up; call player_sheet_open() separately.
void player_play_file(const char *filepath);

// Re-reads the loaded track and repaints the now-playing UI. For callers
// that set up playback through device_state directly (queue jumps, list
// playback).
void player_refresh_now_playing(void);

// Redraws only the artist line, after "Use track artist" has been switched.
void player_refresh_artist(void);

// The "4/12" under the progress bar: the place in the queue as the screen shows
// it (the deal's order under shuffle) and the length. False, with both zero,
// where the screen shows nothing -- one track, a book, DLNA, a stream.
bool player_queue_position(int *position, int *count);

// ---------------------------------------------------------------------------
// The four arrangements of the now-playing page
//
// Standard is the one this player has always had. Waveform moves the title and
// the star onto the sleeve and turns the progress bar into the shape of the
// track, all of it drawn in the sleeve's own colour. Studio centres the sleeve
// with the title across the top and the format under it, over a blurred copy of
// the same artwork filling the screen. Immersive asks for the artwork at the
// shape of the screen and crops it to fill it edge to edge, with the controls
// drawn on a fade over its lower part.
//
// The setting lives in Music > Display options; these are how that page reads
// and writes it. The numbers are what goes in the config, so the first three
// keep the values they have always had.
//
// Which one is actually in force also depends on what is playing: see the note
// on layout_alternative in player.c. Waveform is for a file on the card; Studio,
// Immersive and Poster take whatever is playing, a station included.
//
// The names below are the ones the code has always used; the pills say
// Normale, Waveform, Alternativo, Immersiva and Poster. ALTERNATIVE is the one
// the pill calls Waveform, and STUDIO the one it calls Alternativo.
// ---------------------------------------------------------------------------
typedef enum {
	PLAYER_LAYOUT_STANDARD = 0,
	PLAYER_LAYOUT_ALTERNATIVE = 1,
	PLAYER_LAYOUT_STUDIO = 2,
	PLAYER_LAYOUT_IMMERSIVE = 3, // the sleeve from edge to edge, the controls on a fade over it
	PLAYER_LAYOUT_POSTER = 4,	 // the sleeve at the width of the screen, whole from side to side, the deck below it
} player_layout_t;

player_layout_t player_layout_get(void);
void player_layout_set(player_layout_t layout);

// Whether the play/pause glyph and the played part of the progress bar take
// the playing sleeve's main colour instead of the theme's accent, in every
// arrangement and on every theme (the monochrome palettes too: the switch is
// explicit, so it wins over their one hue while it is on).
// Previous and next keep their own colour either way. Off by default; the
// switch lives beside the layout pills in Music > Display options. Changing
// it only repaints: the colour is the one already worked out from the
// artwork on screen, so nothing is decoded and playback is untouched.
bool player_cover_tint_get(void);
void player_cover_tint_set(bool enabled);

// The output the music was going to has just been unplugged -- a jack pulled
// out, a USB-C DAC removed, headphones that walked out of range. Pauses, so
// that taking the headphones off does not cost three tracks of an album, and
// so that nothing is left playing into a socket with nothing in it.
//
// Does nothing unless something is actually playing. `what` names the output
// for the log and is not shown to anyone. Callers decide whether the output
// that went away was the one in use; this only acts on the decision.
void player_output_unplugged(const char *what);

// ---------------------------------------------------------------------------
// The player as a sheet
//
// The player is not a page you navigate to but a panel that lives just off the
// right edge of the screen and is pulled across with a finger: drag left on
// the Music page and it slides in, drag right and it goes back. That only
// works if it can be drawn on top of whatever is behind it, so it is an object
// on the top layer rather than an LVGL screen.
// ---------------------------------------------------------------------------

// The hardware transport keys, dispatched here from the button threads.
void player_key_play_pause(void);
void player_key_next(void);
void player_key_prev(void);

// What the player's on-screen prev/next buttons do: seconds-sized skips on an
// audiobook or podcast, previous/next track otherwise. The control centre
// calls these so that it behaves exactly like the player.
void player_screen_prev(void);
void player_screen_next(void);

// The feed of the episode being played, for the control centre's star button.
// Returns true and fills id/feed when a podcast is playing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out);

bool player_sheet_is_open(void);

// True while the gesture in progress has been claimed as a sideways drag. The
// tap handlers on the pages behind check this so that swiping the player in
// across a button does not also press it.
bool player_sheet_drag_active(void);
void player_sheet_open(bool animate);
void player_sheet_close(bool animate);

// Makes `obj` a place the sheet can be dragged from. `opening` picks the
// direction: true for a surface behind the sheet (drag left to bring it in),
// false for the sheet itself (drag right to push it away).
void player_sheet_attach_drag(lv_obj_t *obj, bool opening);

// Boot-time "Remember track": loads the remembered track, paused at position.
void player_restore_track(const char *filepath, double position);

// The boot-time restore of a queue that was a library list: the handle takes
// the place of the list of paths, so next/prev work from the first press
// instead of landing back on the remembered track. Takes ownership of `ix`.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position);

// The same for a queue saved as a plain list of paths. `custom` restores the
// queue's own custom flag, which the list of paths on its own cannot carry.
void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position);

// The loaded track's artwork, already decoded and scaled by the player: the
// cover itself, and the blurred copy the controls are drawn on. NULL when the
// track has no artwork (or its decode has not landed yet). Borrowed, not
// owned -- the player frees these at the next track change, so nothing may
// keep them across one.
const lv_image_dsc_t *player_cover_image(void);
const lv_image_dsc_t *player_backdrop_image(void);

// The path that artwork was read from: the track for a file, the downloaded
// icon or cover for a live stream. NULL whenever player_cover_image() is NULL.
// For whoever wants the same picture at another size -- the Framed screensaver
// asks the cover loader for it at the frame's -- without guessing which of the
// several places a picture can come from the player used.
const char *player_cover_path(void);

// Registers a callback run immediately before that artwork is freed, so
// whoever borrowed it can drop its references first. One at a time; NULL
// clears it.
void player_set_cover_release_cb(void (*cb)(void));


#endif
