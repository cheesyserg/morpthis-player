// board_profile.h -- the small set of dimensions that differ on the
// TempoTec V1.
//
// This tree targets the V1 and its 240x320 panel. The profile stays behind a
// query so the pages which were written for a larger panel still compile and
// can be exercised on the host, but on the device there is only one answer.

#ifndef SRC_GUI_BOARD_PROFILE_H_
#define SRC_GUI_BOARD_PROFILE_H_

#include <stdbool.h>

#include "lvgl/lvgl.h"

// True for the V1 profile. BOARD=tempotec_v1 selects it for the host build;
// BOARD_DEFAULT_TEMPOTEC_V1 is defined by the target build, so the device
// always takes it. MORPTHIS_PANEL=240x320 also selects it for host simulation.
bool bp_is_tempotec_v1(void);

int bp_screen_w(void);
int bp_screen_h(void);
int bp_padding(void);
int bp_status_bar_h(void);
int bp_tile_radius(void);

// Common compact-shell geometry.  Callers which already have gui_config_t can
// still key off screen_width < 320; these helpers are for code built before the
// display exists (notably fonts_init()).
int bp_header_button_size(void);
int bp_header_button_gap(void);

// One geometry number written for both panels: `compact` on a 240x320 V1,
// `regular` on the 480-pixel HiBy players.  Pages whose layout is a list of
// pixel sizes (the equaliser columns, the firmware card, the reorder rows)
// read better with this than with a bp_is_tempotec_v1() ternary per line.
int bp_pick(int compact, int regular);

// Fonts used by the most crowded player/list labels.  On the V1 fonts.c also
// maps the remaining large named UI fonts to compact raster sizes, so pages
// which use font_ui_24 directly do not keep 24 physical pixels on a 240-pixel
// display.
const lv_font_t *bp_title_font(void);
const lv_font_t *bp_artist_font(void);
const lv_font_t *bp_time_font(void);
const lv_font_t *bp_tile_label_font(void);
const lv_font_t *bp_queue_label_font(void);
const lv_font_t *bp_format_label_font(void);

#endif // SRC_GUI_BOARD_PROFILE_H_
