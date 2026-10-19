#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *main_menu_screen;

void main_menu_init(gui_config_t *cfg);

// Settings -> Appearance: larger, centered icons with no tile captions on the
// main menu only. Off by default; the setter applies and saves the choice.
bool main_menu_icons_only(void);
void main_menu_set_icons_only(bool enabled);

bool main_menu_browse(void);
void main_menu_set_browse(bool enabled);

#endif
