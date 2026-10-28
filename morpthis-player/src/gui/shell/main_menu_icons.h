#ifndef MAIN_MENU_ICONS_H
#define MAIN_MENU_ICONS_H

#include "lvgl/lvgl.h"

// Exact-size art for the five icon-only home tiles (row-major order).
// Glyphs are A8 masks, tinted with the same semantic colors as the originals;
// Classic keeps the colored illustrations. NULL means an unknown size, for
// which the caller can fall back to the original scalable image.
const lv_image_dsc_t *main_menu_icon_native(int tile, bool glyph, int size);

#endif /* MAIN_MENU_ICONS_H */
