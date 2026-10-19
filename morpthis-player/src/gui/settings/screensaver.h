#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// The screensaver: what the panel shows for a moment when it lights back up.
// The loaded track's artwork fills the screen, and underneath it a strip of
// the blurred cover carries the title, the artist and the time in large
// figures. A chevron pointing up says how to get rid of it: swipe up.
//
// The two palettes added for the V1 -- Tokyo Night and Amber on Black -- draw
// it the other way round: the artwork runs the whole panel and that same strip
// is a translucent field of the palette's own colour laid over its foot, with
// the title, the artist and the clock in the palette's ink. The blurred copy
// is never borrowed there: a field is a colour, not a blur.
//
// The artwork is borrowed from the player, which has already decoded it for
// the current track -- nothing is decoded here, so a wake stays instant.
//
// That is the Fill style, and the borrowed picture is the size of the player's
// sleeve: covering the screensaver's larger area means enlarging it, which on
// the V1 is half again in Dark and Light and twice over under the palettes'
// field. The Framed style (see screensaver_style_t) is the other answer: the
// artwork whole, with margins round it, at one pixel to one.

void screensaver_init(gui_config_t *cfg);

// Config-backed on/off ("screen"/"screensaver"), set from Settings > Display.
// Off is the default.
void screensaver_set_enabled(bool enabled);
bool screensaver_get_enabled(void);

// Where the picture comes from.
//
// Album is the artwork of what is playing, borrowed from the player and free.
// Images is the card's own Screensaver folder, and costs a decode at every
// wake -- which is why it is not the default and why the folder is judged
// before the option can be chosen at all.
typedef enum {
	SCREENSAVER_SOURCE_ALBUM = 0,
	SCREENSAVER_SOURCE_IMAGES,
} screensaver_source_t;

screensaver_source_t screensaver_source(void);
void screensaver_set_source(screensaver_source_t source);

// How the album source draws the artwork. Config-backed
// ("screen"/"screensaver_style"); pictures from the folder are always drawn
// the one way, since they are made for the screen in the first place.
//
// Fill is the arrangement the screensaver has always had, and the default: the
// artwork covers its area, centre-cropped and enlarged to do it -- everything
// above the strip in Dark and Light, the whole panel under the V1 palettes'
// translucent field.
//
// Framed never enlarges. The whole picture sits in a frame with the player's
// rounded corners and margins on every side, above the title, the artist and
// the clock rather than under them, and it is decoded for exactly that frame
// -- from the file, not from the player's copy -- so it is drawn one pixel to
// one. A file that only carries a small picture shows it at its own size. Dark
// and Light lay it on the player's blurred copy of the artwork when there is
// one the size of the screen, which on the V1 is every arrangement but
// Immersive; the palettes lay it on their own page colour.
typedef enum {
	SCREENSAVER_STYLE_FILL = 0,
	SCREENSAVER_STYLE_FRAMED,
} screensaver_style_t;

screensaver_style_t screensaver_style(void);
void screensaver_set_style(screensaver_style_t style);

// Where the card is mounted, for the Screensaver folder below. Called at
// startup and again whenever a card is attached, like every other module that
// keeps a path on the card (see card_databases_attach).
void screensaver_set_card_root(const char *root);

// Whether that folder holds at least one picture this screen can show: a
// baseline (not progressive) JPEG, no wider than the screen and no taller,
// and portrait rather than landscape. Stops at the first one it finds, so the
// answer costs one readable file and not a whole folder.
//
// What the settings page asks before letting Images be chosen: an option that
// would show nothing is worse than an option that is not offered.
bool screensaver_has_images(void);

// Raises it now. Does nothing when the option is off.
void screensaver_show(void);

// What the power code calls both when the panel goes dark (so the frame left
// in the framebuffer is the one the next wake should open on) and when it
// lights up again: raises the screensaver if it is enabled, and refreshes what
// it shows either way -- the track may well have moved on while the screen was
// off.
bool screensaver_prepare(void);

// Lets go of the artwork it borrowed from the player. The player calls this
// through its release hook immediately before freeing those pixels, because
// the screensaver can be up (invisible, behind a blanked panel) while a track
// change happens.
void screensaver_release_artwork(void);
void screensaver_hide(void);
bool screensaver_is_visible(void);

#endif /* SCREENSAVER_H */
