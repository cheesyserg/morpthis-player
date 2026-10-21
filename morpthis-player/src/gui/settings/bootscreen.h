#ifndef BOOTSCREEN_H
#define BOOTSCREEN_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Boot screen centre: the pictures the next power-on can open with.
//
// A gallery of the built-in pictures and whatever the user has put in the
// `.bootscreen` folder at the root of the card, with a large preview of the
// one selected and an Apply button at the foot of the page. The applied
// picture has a section of its own at the top, so it is always clear which
// one the device will draw.
//
// The page is full screen: it hides the status bar while it is up. The back
// chevron is the interface's own, in the top left.
extern lv_obj_t *bootscreen_screen;

void bootscreen_init(gui_config_t *cfg);

// The applied picture's name, for the Appearance row that opens this page:
// "Stock", "Retrospace", or the file name of the custom picture. A static
// buffer, valid until the next call.
const char *bootscreen_applied_name(void);

#endif // BOOTSCREEN_H
