#ifndef GUI_MOREPAGE_H
#define GUI_MOREPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The More page: the door to everything that does not fit on the home screen.
//
// It holds the DAC, and whatever comes next
// goes there too.
//
// It is a section page like Music, Wireless and Streaming: same grid, same
// tiles, same size.

extern lv_obj_t *morepage_screen;

void morepage_init(gui_config_t *cfg);

#endif /* GUI_MOREPAGE_H */
