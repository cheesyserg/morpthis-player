#ifndef ARTISTEXCEPTIONS_H
#define ARTISTEXCEPTIONS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The artists never split (library_artist_exceptions): a list with a delete
// button on each row and a "+" in the corner that asks for a name. Each change
// is saved at once and reported to `changed`, which is how the scan options
// page knows the index has to be filed again.
extern lv_obj_t *artistexceptions_screen;

void artistexceptions_init(gui_config_t *cfg, void (*changed)(void));

#endif /* ARTISTEXCEPTIONS_H */
