#ifndef TOUCHCALPAGE_H
#define TOUCHCALPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The touch calibration page under Developer options: swipe across the middle,
// read the measured band, keep it or restore the built-in values. The values
// go to device_config.ini and are used by the seam compensation at once.
extern lv_obj_t *touchcalpage_screen;

void touchcalpage_init(gui_config_t *cfg);

#endif /* TOUCHCALPAGE_H */
