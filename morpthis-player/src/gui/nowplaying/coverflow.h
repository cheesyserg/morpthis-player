#ifndef COVERFLOW_H
#define COVERFLOW_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// The album carousel: a horizontal row of records. The one in the middle faces
// the user; the ones on either side turn away from it and are walked left and
// right with a finger.
//
// The turn is a perspective projection, not a squeeze: a side record narrows
// towards its inner edge and its artwork crowds naturally towards that edge.
// LVGL cannot draw that projection directly, so coverflow.c works it out per
// pixel. Only the two records either side of the middle are part way through a
// turn, which keeps the work bounded while a finger is down.
//
// What gets turned is the RECORD, not the cover: a square built once per place,
// holding the card and, printed on it, the artwork -- or the album glyph when
// there is none. So the container turns with the picture, the glyph turns with
// the card, and a place whose cover has not arrived shows a blank record rather
// than somebody else's sleeve.
//
// Every cover is still decoded ONCE, at one size, and kept in the thumbnail
// database.
//
// It is not a page but a sheet, like the control centre: it comes up over the
// main menu following the finger, and goes back down the same way.

void coverflow_init(gui_config_t *cfg);

// Brings the sheet up on its own, without a finger on it. Safe to call when the
// library is closed: it says so rather than opening an empty page.
void coverflow_open(void);

// The setting behind it. Off by default: it is a second way to reach the albums,
// not a replacement for the list, and it costs a database of decoded covers.
bool coverflow_enabled(void);
void coverflow_set_enabled(bool on);

// Puts the pull that opens the carousel on a main-menu touch target (the grid
// and its mini-player footer). The gesture starts only in the bottom band of
// the screen and only after a good upward travel, so taps are untouched; while
// the setting is off it refuses, so switching it does not need the menu rebuilt.
//
// The way back out is the grip at the top of the carousel, pulled downwards.
// The page has no chevron and no swipe-back: left and right belong to the row,
// and an arrow drawn over the top record is an arrow in the way of the one
// thing the page is for.
void coverflow_attach_edge(lv_obj_t *target, gui_config_t *cfg);

// True while that pull is being followed, and for one tick afterwards. The
// gesture starts on a card or the mini-player and that target still gets its
// CLICKED when the finger lifts, so its handlers have to ask -- otherwise the
// sheet comes up with Music or Now Playing opened behind it.
bool coverflow_drag_active(void);

#endif /* COVERFLOW_H */
