#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>

// The touchscreen's node, as LVGL opened it.
//
// Declared here, defined in main.c -- that is where touch is opened -- with an
// empty version for the host build. Same shape as display_rotation_supported()
// and friends in power.h, which live there for the same reason (and
// display_get_rotated() is what a caller reading the panel itself needs:
// rotation has to be applied by hand, or the controls come out upside down).

// The evdev node LVGL's indev opened: the one that reports itself as a touch
// panel, or event1 and then event0 when none does. NULL when there is no touch
// device.
const char *panel_touch_device(void);

#endif /* PANEL_H */
