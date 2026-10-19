/* The production widgets and layout, without any display/audio/font libraries. */
#include "../../lv_conf.h"

#undef LV_USE_FREETYPE
#define LV_USE_FREETYPE 0
#undef LV_USE_SDL
#define LV_USE_SDL 0
#undef LV_USE_LINUX_FBDEV
#define LV_USE_LINUX_FBDEV 0
#undef LV_USE_EVDEV
#define LV_USE_EVDEV 0

/* Supply deterministic font metrics instead of opening device font files.
 * Two sizes, so that a page which draws one part of itself in a larger font
 * than the rest can be told apart from one that does not, and so that the
 * layout is measured with both shapes. */
#undef LV_FONT_MONTSERRAT_14
#define LV_FONT_MONTSERRAT_14 1
#undef LV_FONT_MONTSERRAT_20
#define LV_FONT_MONTSERRAT_20 1
