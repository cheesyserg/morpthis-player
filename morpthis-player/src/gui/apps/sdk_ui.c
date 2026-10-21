#include "sdk_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/apps/apphost.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static bool app_is_game(void) {
	return sdk_app && sdk_app->category == APP_CATEGORY_GAME;
}

int morpthis_ui_width(void) {
	return sdk_cfg ? (int)sdk_cfg->screen_width : 240;
}

int morpthis_ui_height(void) {
	if (!sdk_cfg) {
		return 320;
	}
	if (app_is_game()) {
		return (int)sdk_cfg->screen_height;
	}
	return (int)sdk_cfg->screen_height - sdk_content_top();
}

// A game owns the whole panel: the status bar and chevron go away, and the
// edge swipe with them. They come back when the app leaves.
static void game_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	back_gesture_blocked(true);
}

static void game_unloaded_cb(lv_event_t *e) {
	(void)e;
	back_gesture_blocked(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

// The top edge of a game: a downward swipe brings the status bar back for a
// second. The bar is on the top layer then, so its own drag handler takes the
// next swipe and opens the control centre.
static void game_edge_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	if (!sdk_cfg) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		tracking = point.y < sdk_cfg->top_bar_height;
		engaged = false;
		if (tracking) {
			start = point;
		}
		return;
	}
	if (!tracking) {
		return;
	}

	int dy = point.y - start.y;
	if (code == LV_EVENT_PRESSING) {
		if (!engaged && dy > 8) {
			engaged = true;
			topbar_peek();
		}
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		bool was_engaged = engaged;
		tracking = false;
		engaged = false;
		// A flick can end between two reads of the panel, with no PRESSING at
		// all; the release is then the only place the gesture can be seen.
		if (!was_engaged && code == LV_EVENT_RELEASED && dy >= 24) {
			topbar_peek();
		}
	}
}

static void touch_event_cb(lv_event_t *e) {
	morpthis_screen_t *screen = lv_event_get_user_data(e);
	if (!screen || !screen->app || !screen->app->alive || !screen->app->touch_cb) {
		return;
	}

	morpthis_touch_action_t action;
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		action = MORPTHIS_TOUCH_DOWN;
		break;
	case LV_EVENT_PRESSING:
		action = MORPTHIS_TOUCH_MOVE;
		break;
	case LV_EVENT_RELEASED:
		action = MORPTHIS_TOUCH_UP;
		break;
	default:
		action = MORPTHIS_TOUCH_CANCEL;
		break;
	}

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);
	lv_area_t area;
	lv_obj_get_coords(screen->content, &area);

	screen->app->touch_cb(action, (morpthis_point_t){point.x - area.x1, point.y - area.y1},
						  screen->app->touch_user);
}

static bool back_guard(void) {
	morpthis_app_t *app = sdk_app;
	if (!app || !app->alive) {
		return false;
	}

	// The app may answer BACK by asking for the background (a music app that is
	// playing does). The flag is consumed here: when it is set the app has
	// already left its screen, and quitting it now would stop the sound it just
	// asked to keep.
	app->background_requested = false;

	// Let the app see the button first. Then quit regardless: in API 2 the
	// chevron is the way out of an app, and an app that wants an internal back
	// step puts a control on its own screen.
	sdk_dispatch_button(MORPTHIS_BUTTON_BACK, MORPTHIS_INPUT_PRESS);
	sdk_dispatch_button(MORPTHIS_BUTTON_BACK, MORPTHIS_INPUT_RELEASE);
	if (app->background_requested) {
		app->background_requested = false;
		return true;
	}
	morpthis_app_quit();
	return true;
}

static void background_cb(lv_event_t *e) {
	(void)e;
	morpthis_app_background();
}

// The player's way out of a non-game app without closing it. The heading row is
// the player's, so the button cannot collide with anything the app draws.
static void add_background_button(morpthis_screen_t *screen) {
	lv_obj_t *button = lv_btn_create(screen->root);
	settingsrow_place_corner_button(button, sdk_cfg, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_chevron_up);
	// A down chevron: put the app away rather than raise it.
	lv_image_set_rotation(icon, 1800);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, sdk_cfg);
	lv_obj_center(icon);

	lv_obj_add_event_cb(button, background_cb, LV_EVENT_CLICKED, NULL);
}

morpthis_screen_t *morpthis_screen_create(const char *title) {
	if (!sdk_cfg || !sdk_app || !sdk_app->alive) {
		errno = ENODEV;
		return NULL;
	}

	morpthis_screen_t *screen = calloc(1, sizeof(*screen));
	if (!screen) {
		return NULL;
	}
	screen->app = sdk_app;

	screen->root = lv_obj_create(NULL);
	if (!screen->root) {
		free(screen);
		return NULL;
	}
	lv_obj_add_style(screen->root, &theme_style_screen, 0);

	// The player owns the heading row and the space under it. The app's
	// coordinates start below it, so its 0,0 is the first pixel it may draw.
	// A game has no heading row: its 0,0 is the first pixel of the panel.
	const bool game = app_is_game();
	if (!game) {
		screen->title_label = settingsrow_title(screen->root, sdk_cfg, title ? title : "");
		add_background_button(screen);
	}
	screen->content_top = game ? 0 : sdk_content_top();

	screen->content = lv_obj_create(screen->root);
	lv_obj_remove_style_all(screen->content);
	lv_obj_set_pos(screen->content, 0, screen->content_top);
	lv_obj_set_size(screen->content, sdk_cfg->screen_width,
					(int)sdk_cfg->screen_height - screen->content_top);
	lv_obj_set_style_pad_all(screen->content, 0, 0);
	lv_obj_set_style_border_width(screen->content, 0, 0);
	lv_obj_set_style_bg_opa(screen->content, LV_OPA_TRANSP, 0);
	lv_obj_set_scrollable(screen->content, false);
	// The page takes the presses its widgets do not. touch_event_cb hangs off
	// this object; a plain lv_obj is clickable by default, but say so, because
	// an app that draws its own surface has nothing else to catch a tap.
	lv_obj_set_clickable(screen->content, true);

	if (!game) {
		switcher_attach_back_gesture(screen->root);
		switcher_attach_back_gesture(screen->content);
		switcher_set_back_guard(screen->root, back_guard);
	}

	// Raw touch, for an app that draws its own surface. Widgets receive their
	// own taps; the events here are the ones that land on the page itself.
	lv_obj_add_event_cb(screen->content, touch_event_cb, LV_EVENT_PRESSED, screen);
	lv_obj_add_event_cb(screen->content, touch_event_cb, LV_EVENT_PRESSING, screen);
	lv_obj_add_event_cb(screen->content, touch_event_cb, LV_EVENT_RELEASED, screen);
	lv_obj_add_event_cb(screen->content, touch_event_cb, LV_EVENT_PRESS_LOST, screen);

	if (game) {
		// No edge swipe and no chevron: the whole panel belongs to the game.
		// The top edge is the only chrome, and it is ours.
		lv_obj_add_event_cb(screen->root, game_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
		lv_obj_add_event_cb(screen->root, game_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
		lv_obj_add_event_cb(screen->content, game_edge_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(screen->content, game_edge_cb, LV_EVENT_PRESSING, NULL);
		lv_obj_add_event_cb(screen->content, game_edge_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(screen->content, game_edge_cb, LV_EVENT_PRESS_LOST, NULL);
	}

	screen->next = sdk_app->screens;
	sdk_app->screens = screen;
	return screen;
}

void morpthis_screen_set_title(morpthis_screen_t *screen, const char *title) {
	if (screen && screen->title_label) {
		lv_label_set_text(screen->title_label, title ? title : "");
	}
}

void morpthis_screen_load(morpthis_screen_t *screen) {
	if (!screen || !screen->app || !screen->app->alive) {
		return;
	}
	screen->app->active = screen;
	switch_screen(screen->root);
}

morpthis_screen_t *morpthis_screen_active(void) {
	return sdk_app ? sdk_app->active : NULL;
}

void sdk_screen_destroy(morpthis_screen_t *screen) {
	if (!screen) {
		return;
	}

	morpthis_app_t *app = screen->app;
	if (app) {
		morpthis_screen_t **link = &app->screens;
		while (*link && *link != screen) {
			link = &(*link)->next;
		}
		if (*link == screen) {
			*link = screen->next;
		}
		if (app->active == screen) {
			app->active = NULL;
		}
	}

	while (screen->widgets) {
		morpthis_widget_destroy(screen->widgets);
	}
	if (screen->root) {
		switcher_clear_back_guard(screen->root);
		lv_obj_delete(screen->root);
	}
	free(screen);
}

void morpthis_screen_destroy(morpthis_screen_t *screen) {
	if (!screen) {
		return;
	}
	// Deleting the page LVGL is currently drawing would leave the display on a
	// freed object. An app that wants to leave its visible screen calls
	// morpthis_app_quit(); the host tears every screen down from a safe point.
	if (screen->app && screen->app->active == screen) {
		morpthis_app_quit();
		return;
	}
	sdk_screen_destroy(screen);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

static morpthis_widget_t *widget_new(morpthis_screen_t *screen, sdk_widget_kind_t kind) {
	if (!screen || !screen->app || !screen->app->alive) {
		errno = ENODEV;
		return NULL;
	}
	morpthis_widget_t *widget = calloc(1, sizeof(*widget));
	if (!widget) {
		return NULL;
	}
	widget->screen = screen;
	widget->kind = kind;
	widget->visible = true;
	widget->enabled = true;
	widget->next = screen->widgets;
	screen->widgets = widget;
	return widget;
}

static void button_click_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (switcher_back_drag_active()) {
		return; // a swipe across the key, not a tap on it
	}
	if (widget && widget->visible && widget->enabled && widget->click_cb) {
		widget->click_cb(widget, widget->click_user);
	}
}

morpthis_widget_t *morpthis_label_create(morpthis_screen_t *screen, const char *text) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_LABEL);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_label_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	widget->label = widget->obj;
	widget->text = sdk_strdup(text);
	lv_obj_add_style(widget->obj, &theme_style_text, 0);
	lv_obj_set_style_text_font(widget->obj, &font_ui_22, 0);
	lv_label_set_text(widget->obj, widget->text ? widget->text : "");
	return widget;
}

morpthis_widget_t *morpthis_button_create(morpthis_screen_t *screen, const char *text,
										  morpthis_click_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_BUTTON);
	if (!widget) {
		return NULL;
	}

	lv_obj_t *button = lv_btn_create(screen->content);
	if (!button) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	widget->obj = button;
	widget->click_cb = cb;
	widget->click_user = user;
	widget->text = sdk_strdup(text);

	// The same key shape as the keyboard and the settings rows: a card on the
	// page, slightly lifted when pressed.
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, 10, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 2, 0);
	lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
	lv_obj_set_style_shadow_color(button, lv_color_black(), 0);
	lv_obj_set_style_shadow_offset_y(button, 1, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	widget->label = lv_label_create(button);
	lv_obj_add_style(widget->label, &theme_style_text, 0);
	lv_obj_set_style_text_font(widget->label, &font_ui_22, 0);
	lv_label_set_text(widget->label, widget->text ? widget->text : "");
	lv_obj_center(widget->label);

	lv_obj_add_event_cb(button, button_click_cb, LV_EVENT_CLICKED, widget);
	// A swipe-back may start on a key as well as in the gap between keys.
	lv_obj_set_event_bubble(button, true);
	return widget;
}

morpthis_widget_t *morpthis_list_create(morpthis_screen_t *screen) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_LIST);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_obj_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_remove_style_all(widget->obj);
	lv_obj_set_style_pad_all(widget->obj, 0, 0);
	lv_obj_set_style_pad_gap(widget->obj, 6, 0);
	lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(widget->obj, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(widget->obj, LV_SCROLLBAR_MODE_AUTO);
	return widget;
}

static void list_row_cb(lv_event_t *e) {
	morpthis_widget_t *list = lv_event_get_user_data(e);
	lv_obj_t *row = lv_event_get_target(e);
	if (switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}
	if (!list || !list->visible || !list->enabled || !list->select_cb) {
		return;
	}
	int index = (int)(intptr_t)lv_obj_get_user_data(row);
	list->select_cb(list, index, list->select_user);
}

int morpthis_list_add(morpthis_widget_t *list, const char *text) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		errno = EINVAL;
		return -1;
	}

	lv_obj_t *row = lv_btn_create(list->obj);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 48);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 10, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 12, 0);

	lv_obj_t *label = lv_label_create(row);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_label_set_text(label, text ? text : "");
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_set_user_data(row, (void *)(intptr_t)list->item_count);
	lv_obj_add_event_cb(row, list_row_cb, LV_EVENT_CLICKED, list);
	lv_obj_set_event_bubble(row, true);
	return list->item_count++;
}

void morpthis_list_clear(morpthis_widget_t *list) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		return;
	}
	lv_obj_clean(list->obj);
	list->item_count = 0;
}

int morpthis_list_count(const morpthis_widget_t *list) {
	return list && list->kind == SDK_WIDGET_LIST ? list->item_count : 0;
}

void morpthis_list_set_select_cb(morpthis_widget_t *list, morpthis_index_cb cb, void *user) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		return;
	}
	list->select_cb = cb;
	list->select_user = user;
}

static void image_reload(morpthis_widget_t *widget) {
	if (!widget || widget->kind != SDK_WIDGET_IMAGE || !widget->image) {
		return;
	}

	lv_image_set_src(widget->obj, NULL);
	cover_free(&widget->cover);

	char path[APPS_FOLDER_MAX + 256];
	if (sdk_resolve_path(widget->image, path, sizeof(path)) != 0) {
		return;
	}

	int width = widget->width > 0 ? widget->width : 160;
	int height = widget->height > 0 ? widget->height : 160;
	if (cover_load_image_file(path, width, height, COVER_FIT_CONTAIN, &widget->cover)) {
		lv_image_set_src(widget->obj, &widget->cover.dsc);
	}
}

morpthis_widget_t *morpthis_image_create(morpthis_screen_t *screen, const char *relative_path) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_IMAGE);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_image_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}

	// A picture is not a control: a press on it belongs to the page. Otherwise
	// a full-screen image would swallow the page's touch callback.
	lv_obj_set_clickable(widget->obj, false);

	widget->image = sdk_strdup(relative_path);
	// The size is usually set just after creation; the default box keeps an
	// icon visible even when the app never calls set_size(). Decoding is
	// synchronous, so this is for icons and small pictures, not a full-screen
	// photograph on the UI thread.
	image_reload(widget);
	return widget;
}

static void slider_event_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || widget->kind != SDK_WIDGET_SLIDER) {
		return;
	}
	widget->value = lv_slider_get_value(widget->obj);
	if (widget->setting_value) {
		return;
	}
	if (widget->value_cb) {
		widget->value_cb(widget, widget->value, widget->value_user);
	}
}

morpthis_widget_t *morpthis_slider_create(morpthis_screen_t *screen, int min, int max,
										  int value, morpthis_value_cb cb, void *user) {
	if (max < min) {
		errno = EINVAL;
		return NULL;
	}
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_SLIDER);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_slider_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}

	widget->min = min;
	widget->max = max;
	widget->value = value < min ? min : (value > max ? max : value);
	widget->value_cb = cb;
	widget->value_user = user;

	lv_slider_set_range(widget->obj, min, max);
	lv_slider_set_value(widget->obj, widget->value, LV_ANIM_OFF);
	lv_slider_set_mode(widget->obj, LV_SLIDER_MODE_NORMAL);
	lv_obj_add_style(widget->obj, &theme_style_slider_track, LV_PART_MAIN);
	lv_obj_set_style_radius(widget->obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(widget->obj, &theme_style_accent_bg, LV_PART_INDICATOR);
	lv_obj_set_style_radius(widget->obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(widget->obj, lv_color_white(), LV_PART_KNOB);
	theme_apply_slider_knob(widget->obj);
	lv_obj_set_style_pad_all(widget->obj, 8, LV_PART_KNOB);
	lv_obj_add_event_cb(widget->obj, slider_event_cb, LV_EVENT_VALUE_CHANGED, widget);
	return widget;
}

void morpthis_slider_set_value(morpthis_widget_t *slider, int value) {
	if (!slider || slider->kind != SDK_WIDGET_SLIDER) {
		return;
	}
	if (value < slider->min) {
		value = slider->min;
	}
	if (value > slider->max) {
		value = slider->max;
	}

	// Suppress the event LVGL may emit during the set, then deliver exactly one
	// callback whether or not it did: the API promises one per call.
	slider->setting_value = true;
	lv_slider_set_value(slider->obj, value, LV_ANIM_OFF);
	slider->setting_value = false;
	slider->value = value;
	if (slider->value_cb) {
		slider->value_cb(slider, value, slider->value_user);
	}
}

int morpthis_slider_value(const morpthis_widget_t *slider) {
	return slider && slider->kind == SDK_WIDGET_SLIDER ? slider->value : 0;
}

morpthis_widget_t *morpthis_canvas_create(morpthis_screen_t *screen, int width, int height) {
	if (width <= 0 || height <= 0) {
		errno = EINVAL;
		return NULL;
	}

	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_CANVAS);
	if (!widget) {
		return NULL;
	}

	widget->pixels = calloc((size_t)width * (size_t)height, sizeof(uint16_t));
	widget->obj = lv_image_create(screen->content);
	if (!widget->pixels || !widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}

	widget->width = width;
	widget->height = height;
	lv_obj_set_pos(widget->obj, 0, 0);
	lv_obj_set_size(widget->obj, width, height);

	// The canvas covers the page and is what a game's frames are drawn into,
	// but it is not a control: the press has to reach screen->content, where
	// touch_event_cb turns it into MORPTHIS_TOUCH_DOWN/UP for the app.
	lv_obj_set_clickable(widget->obj, false);

	// LVGL caches an image source by the descriptor's address, so the
	// descriptor has to live as long as the widget, not on this stack.
	lv_image_dsc_t *dsc = &widget->dsc;
	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_RGB565;
	dsc->header.w = (uint32_t)width;
	dsc->header.h = (uint32_t)height;
	dsc->header.stride = (uint32_t)width * 2;
	dsc->data_size = (uint32_t)width * (uint32_t)height * 2;
	dsc->data = (const uint8_t *)widget->pixels;
	lv_image_set_src(widget->obj, dsc);

	return widget;
}

uint16_t *morpthis_canvas_buffer(morpthis_widget_t *canvas) {
	if (!canvas || canvas->kind != SDK_WIDGET_CANVAS) {
		return NULL;
	}
	return canvas->pixels;
}

void morpthis_canvas_invalidate(morpthis_widget_t *canvas, int x, int y, int width, int height) {
	if (!canvas || !canvas->obj || canvas->kind != SDK_WIDGET_CANVAS || width <= 0 || height <= 0) {
		return;
	}

	lv_area_t base;
	lv_obj_get_coords(canvas->obj, &base);
	lv_area_t area = {
		.x1 = base.x1 + x,
		.y1 = base.y1 + y,
		.x2 = base.x1 + x + width - 1,
		.y2 = base.y1 + y + height - 1,
	};
	lv_obj_invalidate_area(canvas->obj, &area);
}

void morpthis_ui_refresh(void) {
	// A game draws on its own clock; without this the frame it just wrote waits
	// for the display timer's own phase, which shows as jitter at 60 steps a
	// second. Safe here: a posted callback runs between renders, never inside
	// one.
	lv_refr_now(NULL);
}

void morpthis_widget_destroy(morpthis_widget_t *widget) {
	if (!widget) {
		return;
	}

	morpthis_screen_t *screen = widget->screen;
	if (screen) {
		morpthis_widget_t **link = &screen->widgets;
		while (*link && *link != widget) {
			link = &(*link)->next;
		}
		if (*link == widget) {
			*link = widget->next;
		}
	}

	if (widget->obj) {
		if (widget->kind == SDK_WIDGET_IMAGE || widget->kind == SDK_WIDGET_CANVAS) {
			lv_image_set_src(widget->obj, NULL);
		}
		lv_obj_delete(widget->obj);
	}
	cover_free(&widget->cover);
	free(widget->pixels);
	free(widget->text);
	free(widget->image);
	free(widget);
}

void morpthis_widget_set_text(morpthis_widget_t *widget, const char *text) {
	if (!widget || (widget->kind != SDK_WIDGET_LABEL && widget->kind != SDK_WIDGET_BUTTON)) {
		return;
	}
	char *copy = sdk_strdup(text);
	if (!copy) {
		return;
	}
	free(widget->text);
	widget->text = copy;
	if (widget->label) {
		lv_label_set_text(widget->label, widget->text);
	}
}

const char *morpthis_widget_text(const morpthis_widget_t *widget) {
	return widget && widget->text ? widget->text : "";
}

void morpthis_widget_set_pos(morpthis_widget_t *widget, int x, int y) {
	if (widget && widget->obj) {
		lv_obj_set_pos(widget->obj, x, y);
	}
}

void morpthis_widget_set_size(morpthis_widget_t *widget, int width, int height) {
	if (!widget || !widget->obj || widget->kind == SDK_WIDGET_CANVAS) {
		return;
	}
	widget->width = width < 0 ? 0 : width;
	widget->height = height < 0 ? 0 : height;
	lv_obj_set_size(widget->obj, widget->width, widget->height);
	if (widget->kind == SDK_WIDGET_IMAGE && widget->image) {
		image_reload(widget);
	}
}

void morpthis_widget_set_visible(morpthis_widget_t *widget, bool visible) {
	if (!widget || !widget->obj) {
		return;
	}
	widget->visible = visible;
	lv_obj_set_hidden(widget->obj, !visible);
}

void morpthis_widget_set_rotation(morpthis_widget_t *widget, int degrees) {
	if (!widget || !widget->obj || widget->kind != SDK_WIDGET_IMAGE) {
		return;
	}

	// LVGL turns in tenths of a degree, clockwise, and wants the angle in
	// 0..3599. The pivot is the picture's centre.
	int angle = degrees % 360;
	if (angle < 0) {
		angle += 360;
	}
	lv_image_set_rotation(widget->obj, angle * 10);
}

void morpthis_widget_set_opacity(morpthis_widget_t *widget, int opacity) {
	if (!widget || !widget->obj) {
		return;
	}
	if (opacity < 0) {
		opacity = 0;
	}
	if (opacity > 100) {
		opacity = 100;
	}
	lv_obj_set_style_opa(widget->obj, (lv_opa_t)((opacity * 255 + 50) / 100), 0);
}

void morpthis_widget_raise(morpthis_widget_t *widget) {
	if (!widget || !widget->obj) {
		return;
	}
	lv_obj_move_foreground(widget->obj);
}

void morpthis_widget_set_enabled(morpthis_widget_t *widget, bool enabled) {
	if (!widget || !widget->obj) {
		return;
	}
	widget->enabled = enabled;
	if (enabled) {
		lv_obj_remove_state(widget->obj, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(widget->obj, LV_STATE_DISABLED);
	}
}

bool morpthis_widget_is_visible(const morpthis_widget_t *widget) {
	return widget && widget->visible;
}

bool morpthis_widget_is_enabled(const morpthis_widget_t *widget) {
	return widget && widget->enabled;
}

void morpthis_toast(const char *text) {
	toast_plain(text ? text : "");
}
