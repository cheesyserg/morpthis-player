#ifndef MORPTHIS_TEST_STUB_LVGL_H
#define MORPTHIS_TEST_STUB_LVGL_H

/* The smallest lvgl/lvgl.h that lets a plain-C test compile a player file
 * whose header mentions LVGL only in passing: wifi.c includes power.h for
 * power_screen_is_on(), and power.h names lv_display_t and lv_timer_t in
 * prototypes for display work the Wi-Fi worker never calls. Both are only
 * ever pointed at there, so two incomplete types are the whole contract.
 *
 * Only the wifi test target puts this directory on its include path; the
 * player and the UI tests use the real LVGL. */
typedef struct lv_display lv_display_t;
typedef struct lv_timer lv_timer_t;

#endif /* MORPTHIS_TEST_STUB_LVGL_H */
