#ifndef PICTUREVIEW_H
#define PICTUREVIEW_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A picture on a page of its own: the file manager opens JPEG and PNG files
// here.
extern lv_obj_t *pictureview_screen;

void pictureview_init(gui_config_t *cfg);

// Decodes and shows `path` on the picture page, scaled down to fit when larger
// than the viewport and left at 1:1 when smaller. False, staying where it is,
// when the file cannot be read or is not a readable JPEG or PNG.
bool pictureview_open(const char *path);

#endif /* PICTUREVIEW_H */
