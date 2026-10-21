#ifndef APPSPAGE_H
#define APPSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Apps page: what is installed under <card>/Apps, one row per app.
//
// The page is a list rather than a grid because the number of apps is not
// known when the page is built, and a list scrolls without a layout decision.
// It is rebuilt every time it is opened, so an app copied onto the card (or
// deleted from it) shows up without a restart.
//
// Tapping a row tries the SDK's in-process path first: a shared object with a
// morpthis_app_main() entry becomes a page of its own, with the player's
// top bar, chevron and swipe-back. A headless tool (a regular executable) falls
// back to the separate fork/execve process the page has always run.

extern lv_obj_t *appspage_screen;

void appspage_init(gui_config_t *cfg);

#endif /* APPSPAGE_H */
