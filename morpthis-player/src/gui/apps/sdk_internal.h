#ifndef SDK_INTERNAL_H
#define SDK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "morpthis/morpthis.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/gui.h"
#include "src/system/apps/apps.h"

// The player's side of the SDK. The public types (morpthis_app_t,
// morpthis_screen_t, morpthis_widget_t) are opaque to the app and defined only
// here.

typedef enum {
	SDK_WIDGET_LABEL = 0,
	SDK_WIDGET_BUTTON,
	SDK_WIDGET_LIST,
	SDK_WIDGET_IMAGE,
	SDK_WIDGET_SLIDER,
	SDK_WIDGET_CANVAS,
} sdk_widget_kind_t;

struct morpthis_widget {
	morpthis_screen_t *screen;
	lv_obj_t *obj;
	lv_obj_t *label; // the text child of a label or a button
	sdk_widget_kind_t kind;

	char *text;
	char *image;

	bool visible;
	bool enabled;

	int width;
	int height;

	morpthis_click_cb click_cb;
	void *click_user;
	morpthis_index_cb select_cb;
	void *select_user;
	morpthis_value_cb value_cb;
	void *value_user;

	int min;
	int max;
	int value;
	int item_count;

	// A programmatic slider update may or may not emit LV_EVENT_VALUE_CHANGED
	// depending on the LVGL path taken; these make the app see one callback
	// either way.
	bool setting_value;
	bool value_event_seen;

	cover_image_t cover; // decoded pixels for an image widget

	// A canvas: the app's own RGB565 framebuffer and the image source that
	// describes it. The descriptor lives here because LVGL caches the pointer.
	uint16_t *pixels;
	lv_image_dsc_t dsc;

	struct morpthis_widget *next;
};

struct morpthis_screen {
	morpthis_app_t *app;
	lv_obj_t *root;
	lv_obj_t *content;
	lv_obj_t *title_label;
	int content_top;

	struct morpthis_widget *widgets;
	struct morpthis_screen *next;
};

struct morpthis_app {
	char id[APPS_ID_MAX];
	char name[APPS_NAME_MAX];
	char version[APPS_VERSION_MAX];
	char folder[APPS_FOLDER_MAX];
	app_tier_t tier;
	app_category_t category;

	void *dl_handle;
	bool alive;
	bool quitting;

	// Set by the app through morpthis_input_set_gameplay(); read on the input
	// thread. A game in active play owns the volume keys; in a menu or pause
	// they go back to the player.
	volatile bool gameplay;

	morpthis_screen_t *screens;
	morpthis_screen_t *active;

	morpthis_button_cb button_cb;
	void *button_user;
	morpthis_touch_cb touch_cb;
	void *touch_user;
};

extern morpthis_app_t *sdk_app;
extern gui_config_t *sdk_cfg;

// sdk_app.c
char *sdk_strdup(const char *text);
bool sdk_safe_relative(const char *path);
int sdk_resolve_path(const char *relative, char *out, size_t size);
size_t sdk_mem_available(void);
int sdk_content_top(void);
void sdk_set_app(morpthis_app_t *app);
void sdk_detach_app(morpthis_app_t *app);
void sdk_dispatch_button(morpthis_button_t button, morpthis_input_action_t action);

// sdk_ui.c
void sdk_screen_destroy(morpthis_screen_t *screen);

#endif /* SDK_INTERNAL_H */
