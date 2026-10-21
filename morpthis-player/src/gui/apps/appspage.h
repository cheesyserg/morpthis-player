#ifndef APPSPAGE_H
#define APPSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Apps page: what is installed under <card>/Apps, one tile per app.
//
// The page is the player's tiled menu, like the main menu and More. It is
// rebuilt every time it is opened, so an app copied onto the card (or deleted
// from it) shows up without a restart; only the tiles above the fold are on
// screen at once and the grid scrolls when there are more.
//
// Each tile shows the app's own icon when its manifest names one, and the
// generic apps glyph otherwise. The sentence about where apps live is behind
// the info button in the top right, not printed above the grid.
//
// Tapping a tile tries the SDK's in-process path first: a shared object with a
// morpthis_app_main() entry becomes a page of its own, with the player's
// top bar, chevron and swipe-back. A headless tool (a regular executable) falls
// back to the separate fork/execve process the page has always run.

extern lv_obj_t *appspage_screen;

void appspage_init(gui_config_t *cfg);

#endif /* APPSPAGE_H */
