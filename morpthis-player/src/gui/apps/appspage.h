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
// Tapping a row starts the app's entry binary. Today that is a separate
// process the player waits for -- enough for tools. UI apps need the SDK's
// in-process model; see docs/apps.md.

extern lv_obj_t *appspage_screen;

void appspage_init(gui_config_t *cfg);

#endif /* APPSPAGE_H */
