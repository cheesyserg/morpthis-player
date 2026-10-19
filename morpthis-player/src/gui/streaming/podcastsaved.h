#ifndef PODCASTSAVED_H
#define PODCASTSAVED_H

#include "lvgl.h"
#include "src/gui/shell/gui.h"

// The downloaded podcasts (podcastdl.h): one row per podcast folder, and inside
// it the episodes it holds, newest first. Works with no network at all.
void podcastsaved_init(gui_config_t *cfg);

// The list of podcasts. Read from the card every time it is shown.
extern lv_obj_t *podcastsaved_screen;

#endif /* PODCASTSAVED_H */
