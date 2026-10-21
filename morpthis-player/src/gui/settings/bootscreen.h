#ifndef BOOTSCREEN_H
#define BOOTSCREEN_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Boot screen centre: the pictures the next power-on can open with.
//
// A horizontal carousel of the built-in pictures and whatever the user has put
// in the `.bootscreen` folder at the root of the card. The picture nearest the
// middle is drawn large and the ones either side are smaller, scaling as the
// row is dragged. The applied picture sits in its own column at the far left,
// behind a vertical rule, so it is always clear which one the device will
// draw. Apply is fixed at the foot of the page.
//
// The page keeps the status bar; the back chevron is the interface's own, in
// the top left.
extern lv_obj_t *bootscreen_screen;

void bootscreen_init(gui_config_t *cfg);

// The applied picture's name, for the Appearance row that opens this page:
// "Stock", "Retrospace", or the file name of the custom picture. A static
// buffer, valid until the next call.
const char *bootscreen_applied_name(void);

#endif // BOOTSCREEN_H
