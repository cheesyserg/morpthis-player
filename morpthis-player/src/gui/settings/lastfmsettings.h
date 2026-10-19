#ifndef LASTFMSETTINGS_H
#define LASTFMSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Last.fm page, reached from Music settings, and its sign-in screen. Also
// starts the once-a-second timer that tells src/system/lastfm what is playing.
void lastfmsettings_init(gui_config_t *cfg);

lv_obj_t *lastfmsettings_screen(void);

#endif /* LASTFMSETTINGS_H */
