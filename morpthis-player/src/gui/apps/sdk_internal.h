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
	SDK_WIDGET_CONTAINER,
	SDK_WIDGET_SWITCH,
	SDK_WIDGET_PROGRESS,
	SDK_WIDGET_SPINNER,
	SDK_WIDGET_SEGMENTED,
	SDK_WIDGET_SETTINGS_ROW,
	SDK_WIDGET_TEXT_INPUT,
	SDK_WIDGET_QRCODE,
	SDK_WIDGET_ROLLER,
	SDK_WIDGET_ARC,
} sdk_widget_kind_t;

// One row of an SDK list. The objects are children of the row button; the
// covers are owned here because LVGL only keeps the descriptor pointer.
typedef struct sdk_list_row {
	lv_obj_t *row;
	lv_obj_t *text;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *detail;
	lv_obj_t *chevron;
	lv_obj_t *playmark;
	cover_image_t icon_cover;
} sdk_list_row_t;

struct morpthis_widget {
	morpthis_screen_t *screen;
	lv_obj_t *obj;
	lv_obj_t *label; // the text child of a label or a button
	lv_obj_t *value_label; // a settings row's value on the right
	sdk_widget_kind_t kind;

	char *text;
	char *image;

	// The SDK tree, for containers. `next` is the screen's widget list;
	// `next_sibling` is the parent's child list.
	struct morpthis_widget *parent;
	struct morpthis_widget *children;
	struct morpthis_widget *next_sibling;

	// A text input's prompt title and the app callback its edit reports to.
	morpthis_text_cb text_cb;
	void *text_user;
	bool password;
	bool text_scroll;

	// An icon button's glyph child, and which glyph it is. `icon` is only used
	// by a button; an image widget keeps its picture in `obj`.
	lv_obj_t *icon;
	morpthis_icon_t icon_kind;

	// Slider release (the finger lifted), for a seek bar. Other widgets ignore it.
	morpthis_value_cb release_cb;
	void *release_user;

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

	// A list's rows, kept one allocation per row so a growing array never moves
	// an image descriptor LVGL is pointing at. `playing_index` is the row with
	// the accent bar, or -1.
	sdk_list_row_t **rows;
	int row_capacity;
	int playing_index;

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

	// App-owned actions in the title row, to the left of the player's
	// background button. The title reserves room for 1 + corner_count buttons.
	int corner_count;
};

struct morpthis_app {
	char id[APPS_ID_MAX];
	char name[APPS_NAME_MAX];
	char version[APPS_VERSION_MAX];
	char folder[APPS_FOLDER_MAX]; // the mounted, read-only package root
	char data[APPS_FOLDER_MAX];	  // the writable Apps/merp_data folder
	app_tier_t tier;
	app_category_t category;

	void *dl_handle;
	void (*shutdown)(void); // optional app export, called before unload
	bool alive;
	bool quitting;

	// Set while the app's screens are off display but the app is still loaded
	// (morpthis_app_background). background_requested is the transient flag the
	// back guard consumes: an app that asks for the background from its BACK
	// handler must not then be quit by the chevron that delivered the key.
	bool backgrounded;
	bool background_requested;

	// Set by morpthis_app_back_handled() from the BACK handler: the app moved to
	// one of its own screens, so the guard keeps it loaded instead of quitting.
	bool back_handled;

	// Set by the app through morpthis_input_set_gameplay() and
	// morpthis_input_set_game_menu(); read on the input thread. Active play and
	// the game's own menu both own the volume keys.
	volatile bool gameplay;
	volatile bool game_menu;

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
int sdk_resolve_in(const char *base, const char *relative, char *out, size_t size);
int sdk_resolve_path(const char *relative, char *out, size_t size);
size_t sdk_mem_available(void);
int sdk_content_top(void);
void sdk_set_app(morpthis_app_t *app);
void sdk_detach_app(morpthis_app_t *app);
void sdk_dispatch_button(morpthis_button_t button, morpthis_input_action_t action);

// sdk_audio.c
bool sdk_audio_app_active(void);
bool sdk_audio_app_playing(void);
bool sdk_audio_app_track(morpthis_track_t *out);
void sdk_audio_app_reset(void);

// sdk_playback.c
void sdk_playback_app_reset(void);

// sdk_system.c
void sdk_system_app_reset(void);

// sdk_ui.c
void sdk_screen_destroy(morpthis_screen_t *screen);
void sdk_ui_app_reset(bool teardown);

#endif /* SDK_INTERNAL_H */
