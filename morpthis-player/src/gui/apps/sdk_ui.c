#include "sdk_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/apps/apphost.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"

static const lv_image_dsc_t *icon_for(morpthis_icon_t icon);
static void button_click_cb(lv_event_t *e);
static morpthis_widget_t *widget_new(morpthis_screen_t *screen, sdk_widget_kind_t kind);

// The text-input field whose modal is open, if any. A field destroyed while its
// prompt is up closes the prompt without entering the app.
static morpthis_widget_t *prompt_owner;
static void prompt_abandon(void);
static bool prompt_show(const char *title, const char *initial, bool password, morpthis_text_cb cb,
						void *user, morpthis_widget_t *owner);

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static bool app_is_game(void) {
	return sdk_app && sdk_app->category == APP_CATEGORY_GAME;
}

// The V1's 240 px panel uses the compact row metrics from the player's own
// library; a larger panel gets the roomier ones.
static bool sdk_ui_compact(void) {
	return sdk_cfg && sdk_cfg->screen_width < 320;
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

int morpthis_ui_full_height(void) {
	if (!sdk_cfg) {
		return 320;
	}
	if (app_is_game()) {
		return (int)sdk_cfg->screen_height;
	}
	return (int)sdk_cfg->screen_height - (int)sdk_cfg->top_bar_height;
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
	// playing does) or by moving to another of its own screens (a library walking
	// up a level). Either way it has already left the screen it was on, and
	// quitting now would end work it just asked to keep.
	app->background_requested = false;
	app->back_handled = false;

	// Let the app see the button first. Then quit unless it claimed the step: in
	// API 2 the chevron is the way out of an app, and an app that stays on its
	// own screens says so with morpthis_app_back_handled().
	sdk_dispatch_button(MORPTHIS_BUTTON_BACK, MORPTHIS_INPUT_PRESS);
	sdk_dispatch_button(MORPTHIS_BUTTON_BACK, MORPTHIS_INPUT_RELEASE);
	if (app->background_requested) {
		app->background_requested = false;
		return true;
	}
	if (app->back_handled) {
		app->back_handled = false;
		return true;
	}
	morpthis_app_quit();
	return true;
}

// The swipe-back animates the app's page over a neutral panel, because the app
// decides what the previous page is when BACK is delivered. Showing the
// player's history there would promise a page the app is not going to load.
static bool app_stays_in_page(void) {
	return sdk_app && sdk_app->alive;
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

static morpthis_screen_t *screen_create(const char *title, bool full) {
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
	// A game has no heading row: its 0,0 is the first pixel of the panel. A
	// full screen keeps the status bar but drops the title row.
	const bool game = app_is_game();
	if (!game && !full) {
		screen->title_label = settingsrow_title(screen->root, sdk_cfg, title ? title : "");
		add_background_button(screen);
	}
	screen->content_top = game ? 0 : (full ? (int)sdk_cfg->top_bar_height : sdk_content_top());

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
		switcher_set_back_guard_peek(screen->root, app_stays_in_page);
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

morpthis_screen_t *morpthis_screen_create(const char *title) {
	return screen_create(title, false);
}

morpthis_screen_t *morpthis_screen_create_full(const char *title) {
	return screen_create(title, true);
}

morpthis_widget_t *morpthis_screen_add_corner_button(morpthis_screen_t *screen, morpthis_icon_t icon,
												 morpthis_click_cb cb, void *user) {
	if (!screen || !sdk_cfg || !screen->title_label || app_is_game()) {
		errno = ENOTSUP;
		return NULL;
	}

	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_BUTTON);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_btn_create(screen->root);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}

	// Slot 0 belongs to the player's background button; the app's actions sit
	// to its left, and the title gives up the width.
	settingsrow_place_corner_button(widget->obj, sdk_cfg, 1 + screen->corner_count);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);
	lv_obj_set_style_shadow_width(widget->obj, 0, 0);
	lv_obj_set_style_pad_all(widget->obj, 0, 0);

	widget->icon = lv_image_create(widget->obj);
	lv_image_set_src(widget->icon, icon_for(icon));
	lv_obj_add_style(widget->icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(widget->icon, sdk_cfg);
	lv_obj_center(widget->icon);

	widget->icon_kind = icon;
	widget->click_cb = cb;
	widget->click_user = user;
	lv_obj_add_event_cb(widget->obj, button_click_cb, LV_EVENT_CLICKED, widget);

	screen->corner_count++;
	settingsrow_title_corner_slots(screen->title_label, sdk_cfg, 1 + screen->corner_count);
	return widget;
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
	// An app's pages are not steps in the player's own history: the app's BACK
	// handler chooses what "back" means, and the player must not pop to a page
	// the app has already left. The swipe and the chevron both arrive as BACK.
	switch_screen_no_history(screen->root);
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

static const lv_image_dsc_t *icon_for(morpthis_icon_t icon) {
	switch (icon) {
	case MORPTHIS_ICON_PLAY:
		return &icon_play_large;
	case MORPTHIS_ICON_PAUSE:
		return &icon_pause_large;
	case MORPTHIS_ICON_PREV:
		return &icon_skip_back_large;
	case MORPTHIS_ICON_NEXT:
		return &icon_skip_forward_large;
	case MORPTHIS_ICON_STOP:
		return &icon_stop;
	case MORPTHIS_ICON_QUEUE:
		return &icon_list_queue;
	case MORPTHIS_ICON_MORE:
		return &icon_ellipsis_vertical;
	case MORPTHIS_ICON_SEARCH:
		return &icon_search;
	case MORPTHIS_ICON_CLOSE:
		return &icon_close;
	case MORPTHIS_ICON_CHEVRON_LEFT:
		return &icon_chevron_left;
	case MORPTHIS_ICON_CHEVRON_RIGHT:
		return &icon_chevron_right;
	case MORPTHIS_ICON_CHEVRON_UP:
		return &icon_chevron_up;
	case MORPTHIS_ICON_ALBUM:
		return &icon_album;
	case MORPTHIS_ICON_ARTIST:
		return &icon_artist;
	case MORPTHIS_ICON_FOLDER:
		return &icon_folder;
	case MORPTHIS_ICON_FILE:
		return &icon_file;
	case MORPTHIS_ICON_MUSIC:
		return &icon_music_note;
	case MORPTHIS_ICON_PLAYLIST:
		return &icon_menu_playlist;
	case MORPTHIS_ICON_HEART:
		// The 32 px outline, not icon_heart: that one is the 160 px Easter-egg
		// artwork, and an icon button does not scale its source. Centred in a
		// 56 px button it shows only the middle of a white heart -- a square.
		return &icon_heart_outline;
	case MORPTHIS_ICON_HEART_FILLED:
		return &icon_heart_filled;
	case MORPTHIS_ICON_STAR:
		return &icon_star;
	case MORPTHIS_ICON_STAR_FILLED:
		return &icon_star_filled;
	case MORPTHIS_ICON_SHUFFLE:
		return &icon_shuffle;
	case MORPTHIS_ICON_REPEAT_ALL:
		return &icon_repeat_all;
	case MORPTHIS_ICON_REPEAT_ONE:
		return &icon_repeat_one;
	case MORPTHIS_ICON_SORT:
		return &icon_sort_az;
	case MORPTHIS_ICON_CHECK:
		return &icon_check;
	case MORPTHIS_ICON_PLUS:
		return &icon_plus;
	case MORPTHIS_ICON_DELETE:
		return &icon_trash;
	case MORPTHIS_ICON_REFRESH:
		return &icon_refresh;
	case MORPTHIS_ICON_INFO:
		return &icon_info;
	case MORPTHIS_ICON_ALERT:
		return &icon_circle_alert;
	case MORPTHIS_ICON_WIFI:
		return &icon_wifi;
	case MORPTHIS_ICON_BLUETOOTH:
		return &icon_bluetooth;
	case MORPTHIS_ICON_BATTERY:
		return &icon_battery;
	case MORPTHIS_ICON_CHARGING:
		return &icon_battery_charging_bolt;
	case MORPTHIS_ICON_EQUALIZER:
		return &icon_equalizer;
	case MORPTHIS_ICON_SETTINGS:
		return &icon_menu_settings;
	case MORPTHIS_ICON_SLEEP:
		return &icon_sleep_music_quick;
	case MORPTHIS_ICON_SUN:
		return &icon_sun;
	case MORPTHIS_ICON_VOLUME_HIGH:
		return &icon_volume_high;
	case MORPTHIS_ICON_VOLUME_LOW:
		return &icon_volume_low;
	case MORPTHIS_ICON_VOLUME_MUTE:
		return &icon_volume_mute;
	case MORPTHIS_ICON_HEADPHONES:
		return &icon_headphones;
	case MORPTHIS_ICON_POWER:
		return &icon_power_off;
	case MORPTHIS_ICON_REBOOT:
		return &icon_reboot;
	default:
		return &icon_music2;
	}
}

static void flat_button_style(lv_obj_t *button) {
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
}

morpthis_widget_t *morpthis_icon_button_create(morpthis_screen_t *screen, morpthis_icon_t icon,
											 int size, morpthis_click_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_BUTTON);
	if (!widget) {
		return NULL;
	}

	widget->obj = lv_btn_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	int side = size > 0 ? size : (sdk_ui_compact() ? 44 : 56);
	lv_obj_set_size(widget->obj, side, side);
	flat_button_style(widget->obj);

	widget->icon = lv_image_create(widget->obj);
	lv_image_set_src(widget->icon, icon_for(icon));
	lv_obj_add_style(widget->icon, &theme_style_icon, 0);
	if (sdk_ui_compact() && side < 56) {
		lv_image_set_scale(widget->icon, 192);
	}
	lv_obj_center(widget->icon);

	widget->icon_kind = icon;
	widget->click_cb = cb;
	widget->click_user = user;
	lv_obj_add_event_cb(widget->obj, button_click_cb, LV_EVENT_CLICKED, widget);
	lv_obj_set_event_bubble(widget->obj, true);
	return widget;
}

void morpthis_widget_set_icon(morpthis_widget_t *widget, morpthis_icon_t icon) {
	if (!widget || widget->kind != SDK_WIDGET_BUTTON || !widget->icon) {
		return;
	}
	widget->icon_kind = icon;
	lv_image_set_src(widget->icon, icon_for(icon));
}

static lv_obj_t *text_target(morpthis_widget_t *widget) {
	if (!widget) {
		return NULL;
	}
	if (widget->kind == SDK_WIDGET_LABEL) {
		return widget->obj;
	}
	if (widget->kind == SDK_WIDGET_BUTTON) {
		return widget->label;
	}
	if (widget->kind == SDK_WIDGET_SETTINGS_ROW) {
		return widget->label;
	}
	if (widget->kind == SDK_WIDGET_TEXT_INPUT) {
		return widget->obj;
	}
	return NULL;
}

void morpthis_widget_set_text_size(morpthis_widget_t *widget, int size) {
	lv_obj_t *target = text_target(widget);
	if (!target) {
		return;
	}
	const lv_font_t *font;
	if (size <= 14) {
		font = &font_ui_14;
	} else if (size <= 16) {
		font = &font_ui_16;
	} else if (size <= 18) {
		font = &font_ui_18;
	} else if (size <= 20) {
		font = &font_ui_20;
	} else if (size <= 22) {
		font = &font_ui_22;
	} else if (size <= 24) {
		font = &font_ui_24;
	} else if (size <= 26) {
		font = &font_ui_26;
	} else {
		font = &font_ui_32;
	}
	lv_obj_set_style_text_font(target, font, 0);
}

void morpthis_widget_set_text_dim(morpthis_widget_t *widget, bool dim) {
	lv_obj_t *target = text_target(widget);
	if (!target) {
		return;
	}
	// The shared dim style follows a theme change; a local colour would not.
	lv_obj_remove_style(target, &theme_style_text_dim, 0);
	if (dim) {
		lv_obj_add_style(target, &theme_style_text_dim, 0);
	}
}

void morpthis_widget_set_text_align(morpthis_widget_t *widget, morpthis_text_align_t align) {
	lv_obj_t *target = text_target(widget);
	if (!target) {
		return;
	}
	lv_text_align_t value = align == MORPTHIS_TEXT_CENTER   ? LV_TEXT_ALIGN_CENTER
							: align == MORPTHIS_TEXT_RIGHT ? LV_TEXT_ALIGN_RIGHT
												 : LV_TEXT_ALIGN_LEFT;
	lv_obj_set_style_text_align(target, value, 0);
}

void morpthis_widget_set_text_wrap(morpthis_widget_t *widget, bool wrap) {
	lv_obj_t *target = text_target(widget);
	if (!target) {
		return;
	}
	lv_label_set_long_mode(target, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
}

void morpthis_widget_set_text_scroll(morpthis_widget_t *widget, bool scroll) {
	lv_obj_t *target = text_target(widget);
	if (!target || (widget->kind != SDK_WIDGET_LABEL && widget->kind != SDK_WIDGET_BUTTON)) {
		return;
	}
	widget->text_scroll = scroll;
	if (scroll) {
		scrolltext_apply(target);
		scrolltext_set(target, widget->text ? widget->text : "");
	} else {
		lv_label_set_long_mode(target, LV_LABEL_LONG_CLIP);
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

static void switch_changed_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || !widget->obj || !widget->visible || !widget->enabled) {
		return;
	}
	widget->value = lv_obj_has_state(widget->obj, LV_STATE_CHECKED) ? 1 : 0;
	if (widget->value_cb) {
		widget->value_cb(widget, widget->value, widget->value_user);
	}
}

morpthis_widget_t *morpthis_switch_create(morpthis_screen_t *screen, bool on,
										  morpthis_value_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_SWITCH);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_switch_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	theme_apply_switch(widget->obj);
	lv_obj_set_size(widget->obj, 44, 24);
	if (on) {
		lv_obj_add_state(widget->obj, LV_STATE_CHECKED);
	}
	widget->value = on ? 1 : 0;
	widget->value_cb = cb;
	widget->value_user = user;
	lv_obj_add_event_cb(widget->obj, switch_changed_cb, LV_EVENT_VALUE_CHANGED, widget);
	return widget;
}

bool morpthis_switch_is_on(const morpthis_widget_t *widget) {
	return widget && widget->kind == SDK_WIDGET_SWITCH && widget->value != 0;
}

void morpthis_switch_set_on(morpthis_widget_t *widget, bool on) {
	if (!widget || widget->kind != SDK_WIDGET_SWITCH || !widget->obj) {
		return;
	}
	widget->value = on ? 1 : 0;
	if (on) {
		lv_obj_add_state(widget->obj, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(widget->obj, LV_STATE_CHECKED);
	}
}

morpthis_widget_t *morpthis_progress_create(morpthis_screen_t *screen, int min, int max, int value) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_PROGRESS);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_bar_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_set_width(widget->obj, lv_pct(100));
	lv_obj_set_height(widget->obj, 8);
	lv_obj_set_style_radius(widget->obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(widget->obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(widget->obj, &theme_style_slider_track, LV_PART_MAIN);
	lv_obj_add_style(widget->obj, &theme_style_accent_bg, LV_PART_INDICATOR);
	morpthis_progress_set_range(widget, min, max);
	morpthis_progress_set_value(widget, value);
	return widget;
}

void morpthis_progress_set_value(morpthis_widget_t *widget, int value) {
	if (!widget || widget->kind != SDK_WIDGET_PROGRESS || !widget->obj) {
		return;
	}
	if (value < widget->min) {
		value = widget->min;
	}
	if (value > widget->max) {
		value = widget->max;
	}
	widget->value = value;
	lv_bar_set_value(widget->obj, value, LV_ANIM_OFF);
}

int morpthis_progress_value(const morpthis_widget_t *widget) {
	return widget && widget->kind == SDK_WIDGET_PROGRESS ? widget->value : 0;
}

void morpthis_progress_set_range(morpthis_widget_t *widget, int min, int max) {
	if (!widget || widget->kind != SDK_WIDGET_PROGRESS || !widget->obj) {
		return;
	}
	if (max < min) {
		int swap = min;
		min = max;
		max = swap;
	}
	widget->min = min;
	widget->max = max;
	lv_bar_set_range(widget->obj, min, max);
	morpthis_progress_set_value(widget, widget->value);
}

morpthis_widget_t *morpthis_spinner_create(morpthis_screen_t *screen) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_SPINNER);
	if (!widget) {
		return NULL;
	}
	widget->obj = spinner_create(screen->content, &icon_loader_small);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	return widget;
}

static void arc_changed_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || widget->kind != SDK_WIDGET_ARC || !widget->obj || widget->setting_value) {
		return;
	}
	widget->value = lv_arc_get_value(widget->obj);
	if (widget->value_cb) {
		widget->value_cb(widget, widget->value, widget->value_user);
	}
}

morpthis_widget_t *morpthis_arc_create(morpthis_screen_t *screen, int min, int max, int value,
									   morpthis_value_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_ARC);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_arc_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_set_size(widget->obj, sdk_ui_compact() ? 64 : 96, sdk_ui_compact() ? 64 : 96);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);
	lv_obj_set_style_arc_width(widget->obj, sdk_ui_compact() ? 6 : 8, LV_PART_MAIN);
	lv_obj_set_style_arc_width(widget->obj, sdk_ui_compact() ? 6 : 8, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(widget->obj, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_arc_opa(widget->obj, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_arc_color(widget->obj, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_border_width(widget->obj, 2, LV_PART_KNOB);
	lv_obj_set_style_border_color(widget->obj, theme()->accent, LV_PART_KNOB);
	lv_arc_set_bg_angles(widget->obj, 0, 360);
	widget->value_cb = cb;
	widget->value_user = user;
	morpthis_arc_set_range(widget, min, max);
	morpthis_arc_set_value(widget, value);
	lv_obj_add_event_cb(widget->obj, arc_changed_cb, LV_EVENT_VALUE_CHANGED, widget);
	return widget;
}

void morpthis_arc_set_value(morpthis_widget_t *widget, int value) {
	if (!widget || widget->kind != SDK_WIDGET_ARC || !widget->obj) {
		return;
	}
	if (value < widget->min) {
		value = widget->min;
	}
	if (value > widget->max) {
		value = widget->max;
	}
	widget->setting_value = true;
	lv_arc_set_value(widget->obj, value);
	widget->setting_value = false;
	widget->value = value;
}

int morpthis_arc_value(const morpthis_widget_t *widget) {
	return widget && widget->kind == SDK_WIDGET_ARC ? widget->value : 0;
}

void morpthis_arc_set_range(morpthis_widget_t *widget, int min, int max) {
	if (!widget || widget->kind != SDK_WIDGET_ARC || !widget->obj) {
		return;
	}
	if (max < min) {
		int swap = min;
		min = max;
		max = swap;
	}
	widget->min = min;
	widget->max = max;
	lv_arc_set_range(widget->obj, min, max);
	int value = widget->value;
	if (value < min) {
		value = min;
	}
	if (value > max) {
		value = max;
	}
	widget->setting_value = true;
	lv_arc_set_value(widget->obj, value);
	widget->setting_value = false;
	widget->value = value;
}

morpthis_widget_t *morpthis_container_create(morpthis_screen_t *screen) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_CONTAINER);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_obj_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_remove_style_all(widget->obj);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);
	lv_obj_set_style_pad_all(widget->obj, 0, 0);
	lv_obj_set_scrollable(widget->obj, false);
	lv_obj_set_width(widget->obj, lv_pct(100));
	lv_obj_set_height(widget->obj, LV_SIZE_CONTENT);
	return widget;
}

static lv_flex_align_t flex_align(morpthis_align_t align) {
	switch (align) {
	case MORPTHIS_ALIGN_CENTER:
		return LV_FLEX_ALIGN_CENTER;
	case MORPTHIS_ALIGN_END:
		return LV_FLEX_ALIGN_END;
	case MORPTHIS_ALIGN_SPACE_BETWEEN:
		return LV_FLEX_ALIGN_SPACE_BETWEEN;
	case MORPTHIS_ALIGN_START:
	default:
		return LV_FLEX_ALIGN_START;
	}
}

void morpthis_container_set_layout(morpthis_widget_t *container, morpthis_layout_t layout,
								   morpthis_align_t main_align, morpthis_align_t cross_align, int gap) {
	if (!container || container->kind != SDK_WIDGET_CONTAINER || !container->obj) {
		return;
	}
	if (layout == MORPTHIS_LAYOUT_ROW || layout == MORPTHIS_LAYOUT_COLUMN) {
		lv_obj_set_flex_flow(container->obj, layout == MORPTHIS_LAYOUT_ROW ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(container->obj, flex_align(main_align), flex_align(cross_align), LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_gap(container->obj, gap > 0 ? gap : 0, 0);
	} else {
		lv_obj_set_layout(container->obj, LV_LAYOUT_NONE);
		lv_obj_set_style_pad_gap(container->obj, 0, 0);
	}
}

void morpthis_container_set_scrollable(morpthis_widget_t *container, bool vertical) {
	if (!container || container->kind != SDK_WIDGET_CONTAINER || !container->obj) {
		return;
	}
	lv_obj_set_scrollable(container->obj, vertical);
	lv_obj_set_scroll_dir(container->obj, vertical ? LV_DIR_VER : LV_DIR_NONE);
	lv_obj_set_scrollbar_mode(container->obj, vertical ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
}

static void segment_paint(morpthis_widget_t *widget) {
	if (!widget || widget->kind != SDK_WIDGET_SEGMENTED || !widget->obj) {
		return;
	}
	for (int i = 0; i < widget->item_count; i++) {
		lv_obj_t *pill = lv_obj_get_child(widget->obj, i);
		if (!pill) {
			continue;
		}
		bool selected = i == widget->value;
		lv_obj_set_style_bg_color(pill, selected ? theme()->accent : theme()->surface, 0);
		lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
		lv_obj_t *label = lv_obj_get_child(pill, 0);
		if (label) {
			lv_obj_set_style_text_color(label, selected ? theme_on_accent() : theme()->text_primary, 0);
		}
	}
}

static void segment_clicked_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || !widget->visible || !widget->enabled) {
		return;
	}
	lv_obj_t *pill = lv_event_get_target(e);
	int index = (int)lv_obj_get_index(pill);
	if (index < 0 || index >= widget->item_count) {
		return;
	}
	widget->value = index;
	segment_paint(widget);
	if (widget->value_cb) {
		widget->value_cb(widget, index, widget->value_user);
	}
}

morpthis_widget_t *morpthis_segmented_create(morpthis_screen_t *screen, const char *const *labels,
										 int count, int selected, morpthis_value_cb cb, void *user) {
	if (!labels || count <= 0) {
		return NULL;
	}
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_SEGMENTED);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_obj_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_remove_style_all(widget->obj);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);
	lv_obj_set_style_pad_all(widget->obj, 0, 0);
	lv_obj_set_style_pad_gap(widget->obj, 6, 0);
	lv_obj_set_scrollable(widget->obj, false);
	lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(widget->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_width(widget->obj, lv_pct(100));
	lv_obj_set_height(widget->obj, LV_SIZE_CONTENT);

	widget->value_cb = cb;
	widget->value_user = user;
	widget->value = selected >= 0 && selected < count ? selected : -1;
	widget->item_count = 0;
	for (int i = 0; i < count; i++) {
		lv_obj_t *pill = lv_btn_create(widget->obj);
		if (!pill) {
			break;
		}
		lv_obj_set_height(pill, sdk_ui_compact() ? 32 : 44);
		lv_obj_set_style_pad_hor(pill, sdk_ui_compact() ? 10 : 18, 0);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, labels[i] ? labels[i] : "");
		lv_obj_set_style_text_font(label, sdk_ui_compact() ? &font_ui_16 : &font_ui_20, 0);
		lv_obj_center(label);
		lv_obj_add_event_cb(pill, segment_clicked_cb, LV_EVENT_CLICKED, widget);
		widget->item_count++;
	}
	segment_paint(widget);
	return widget;
}

void morpthis_segmented_set_selected(morpthis_widget_t *widget, int index) {
	if (!widget || widget->kind != SDK_WIDGET_SEGMENTED) {
		return;
	}
	widget->value = index >= 0 && index < widget->item_count ? index : -1;
	segment_paint(widget);
}

int morpthis_segmented_selected(const morpthis_widget_t *widget) {
	return widget && widget->kind == SDK_WIDGET_SEGMENTED ? widget->value : -1;
}

static void settings_row_click_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || !widget->visible || !widget->enabled) {
		return;
	}
	if (widget->click_cb) {
		widget->click_cb(widget, widget->click_user);
	}
}

morpthis_widget_t *morpthis_settings_row_create(morpthis_screen_t *screen, const char *title,
										  const char *detail, const char *value,
										  morpthis_click_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_SETTINGS_ROW);
	if (!widget) {
		return NULL;
	}
	widget->obj = settingsrow_add(screen->content, title ? title : "", &widget->value_label,
								  cb ? settings_row_click_cb : NULL, widget);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	widget->label = settingsrow_name_label(widget->obj);
	widget->text = sdk_strdup(title);
	widget->click_cb = cb;
	widget->click_user = user;
	if (detail && detail[0]) {
		settingsrow_add_caption(widget->obj, detail);
	}

	// The value label and the chevron share the right edge: a row that has a
	// value shows it, one that opens a page shows the chevron.
	widget->icon = lv_image_create(widget->obj);
	if (widget->icon) {
		lv_image_set_src(widget->icon, &icon_chevron_right);
		lv_obj_add_style(widget->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(widget->icon, LV_OPA_60, 0);
		lv_obj_align(widget->icon, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	morpthis_settings_row_set_value(widget, value);
	return widget;
}

void morpthis_settings_row_set_value(morpthis_widget_t *widget, const char *value) {
	if (!widget || widget->kind != SDK_WIDGET_SETTINGS_ROW) {
		return;
	}
	bool has_value = value && value[0];
	if (widget->value_label) {
		lv_label_set_text(widget->value_label, has_value ? value : "");
		lv_obj_set_hidden(widget->value_label, !has_value);
	}
	if (widget->icon) {
		lv_obj_set_hidden(widget->icon, has_value);
	}
}

static void text_input_result_cb(bool accepted, const char *text, void *user);

static void text_input_clicked_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || !widget->obj || !widget->visible || !widget->enabled) {
		return;
	}
	const char *current = lv_textarea_get_text(widget->obj);
	if (!prompt_show(widget->text ? widget->text : "Edit", current, widget->password,
					 text_input_result_cb, widget, widget)) {
		prompt_owner = NULL;
	}
}

static void text_input_result_cb(bool accepted, const char *text, void *user) {
	morpthis_widget_t *widget = user;
	if (!widget || widget->kind != SDK_WIDGET_TEXT_INPUT) {
		return;
	}
	if (!accepted) {
		// The prompt reports an empty string when the user backs out; the field
		// still holds what it held before.
		if (widget->text_cb) {
			widget->text_cb(false, lv_textarea_get_text(widget->obj), widget->text_user);
		}
		return;
	}
	if (widget->obj && text) {
		lv_textarea_set_text(widget->obj, text);
	}
	if (widget->text_cb) {
		widget->text_cb(true, text ? text : "", widget->text_user);
	}
}

morpthis_widget_t *morpthis_text_input_create(morpthis_screen_t *screen, const char *title,
										  const char *initial, bool password,
										  morpthis_text_cb cb, void *user) {
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_TEXT_INPUT);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_textarea_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_obj_set_width(widget->obj, lv_pct(100));
	lv_obj_set_height(widget->obj, sdk_ui_compact() ? 36 : 48);
	lv_textarea_set_one_line(widget->obj, true);
	lv_textarea_set_text(widget->obj, initial ? initial : "");
	lv_obj_set_style_bg_color(widget->obj, theme()->surface, 0);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(widget->obj, theme()->text_primary, 0);
	lv_obj_set_style_text_font(widget->obj, sdk_ui_compact() ? &font_ui_16 : &font_ui_20, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);
	lv_obj_set_style_radius(widget->obj, 8, 0);
	lv_obj_set_style_pad_all(widget->obj, 6, 0);
	if (password) {
		lv_textarea_set_password_mode(widget->obj, true);
		keyboard_style_password(widget->obj, 0);
	}
	widget->text = sdk_strdup(title);
	widget->text_cb = cb;
	widget->text_user = user;
	widget->password = password;
	lv_obj_add_event_cb(widget->obj, text_input_clicked_cb, LV_EVENT_CLICKED, widget);
	return widget;
}

void morpthis_text_input_set_text(morpthis_widget_t *widget, const char *text) {
	if (!widget || widget->kind != SDK_WIDGET_TEXT_INPUT || !widget->obj) {
		return;
	}
	lv_textarea_set_text(widget->obj, text ? text : "");
}

const char *morpthis_text_input_get_text(const morpthis_widget_t *widget) {
	return widget && widget->kind == SDK_WIDGET_TEXT_INPUT && widget->obj ? lv_textarea_get_text(widget->obj) : "";
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
	lv_obj_set_style_pad_gap(widget->obj, sdk_ui_compact() ? 5 : 8, 0);
	lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(widget->obj, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(widget->obj, LV_SCROLLBAR_MODE_AUTO);
	widget->playing_index = -1;
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

int morpthis_list_add_entry(morpthis_widget_t *list, const char *title, const char *detail,
						const char *icon, bool chevron) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		errno = EINVAL;
		return -1;
	}

	if (list->item_count >= list->row_capacity) {
		int capacity = list->row_capacity ? list->row_capacity * 2 : 16;
		sdk_list_row_t **rows = realloc(list->rows, sizeof(*rows) * (size_t)capacity);
		if (!rows) {
			return -1;
		}
		list->rows = rows;
		list->row_capacity = capacity;
	}

	sdk_list_row_t *row = calloc(1, sizeof(*row));
	if (!row) {
		return -1;
	}

	const bool compact = sdk_ui_compact();
	const int height = compact ? 44 : 80;
	const int pad = compact ? 4 : 8;
	const int thumb = compact ? 36 : 56;
	const int radius = compact ? 10 : 12;
	const lv_font_t *title_font = compact ? &font_ui_18 : &font_ui_24;
	const lv_font_t *detail_font = compact ? &font_ui_14 : &font_ui_20;

	// The same row the player's own library draws: a card, a leading thumb,
	// a title over a dim detail, and the chevron on anything that opens a page.
	row->row = lv_btn_create(list->obj);
	if (!row->row) {
		free(row);
		return -1;
	}
	lv_obj_set_width(row->row, lv_pct(100));
	lv_obj_set_height(row->row, height);
	lv_obj_add_style(row->row, &theme_style_card, 0);
	lv_obj_add_style(row->row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row->row, radius, 0);
	lv_obj_set_style_border_width(row->row, 0, 0);
	lv_obj_set_style_shadow_width(row->row, 0, 0);
	lv_obj_set_style_pad_all(row->row, pad, 0);
	lv_obj_set_style_pad_column(row->row, pad, 0);
	lv_obj_set_flex_flow(row->row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row->row, true);

	if (icon && icon[0]) {
		char path[APPS_FOLDER_MAX + 256];
		if (morpthis_fs_resolve(icon, path, sizeof(path)) &&
			cover_load_image_file(path, thumb, thumb, COVER_FIT_COVER, &row->icon_cover)) {
			row->icon = lv_image_create(row->row);
			lv_obj_set_size(row->icon, thumb, thumb);
			lv_image_set_src(row->icon, &row->icon_cover.dsc);
			lv_obj_set_clickable(row->icon, false);
		}
	}

	// Title and detail in a column so the detail cannot eat the title's width.
	row->text = lv_obj_create(row->row);
	lv_obj_remove_style_all(row->text);
	lv_obj_set_flex_grow(row->text, 1);
	lv_obj_set_height(row->text, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row->text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row->text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_all(row->text, 0, 0);
	lv_obj_set_style_pad_row(row->text, 0, 0);
	lv_obj_set_scrollable(row->text, false);
	lv_obj_set_clickable(row->text, false);
	lv_obj_set_event_bubble(row->text, true);

	row->title = lv_label_create(row->text);
	lv_label_set_text(row->title, title ? title : "");
	lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->title, lv_pct(100));
	lv_obj_set_height(row->title, lv_font_get_line_height(title_font));
	lv_obj_add_style(row->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(row->title, title_font, 0);
	lv_obj_set_clickable(row->title, false);

	row->detail = lv_label_create(row->text);
	lv_label_set_text(row->detail, detail ? detail : "");
	lv_label_set_long_mode(row->detail, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->detail, lv_pct(100));
	lv_obj_set_height(row->detail, lv_font_get_line_height(detail_font));
	lv_obj_add_style(row->detail, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->detail, detail_font, 0);
	lv_obj_set_clickable(row->detail, false);
	lv_obj_set_hidden(row->detail, !(detail && detail[0]));

	// The playing bar sits in the row's padding, out of the flex layout, so a
	// row does not move when it becomes the playing one.
	row->playmark = lv_obj_create(row->row);
	lv_obj_set_ignore_layout(row->playmark, true);
	lv_obj_set_size(row->playmark, compact ? 4 : 6, compact ? 26 : 52);
	lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(row->playmark, 0, 0);
	lv_obj_set_style_shadow_width(row->playmark, 0, 0);
	lv_obj_set_style_pad_all(row->playmark, 0, 0);
	lv_obj_set_scrollable(row->playmark, false);
	lv_obj_set_clickable(row->playmark, false);
	lv_obj_set_hidden(row->playmark, true);

	if (chevron) {
		row->chevron = lv_image_create(row->row);
		lv_image_set_src(row->chevron, &icon_chevron_right);
		lv_obj_add_style(row->chevron, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(row->chevron, LV_OPA_60, 0);
		lv_obj_set_clickable(row->chevron, false);
	}

	lv_obj_set_user_data(row->row, (void *)(intptr_t)list->item_count);
	lv_obj_add_event_cb(row->row, list_row_cb, LV_EVENT_CLICKED, list);

	list->rows[list->item_count] = row;
	return list->item_count++;
}

int morpthis_list_add(morpthis_widget_t *list, const char *text) {
	return morpthis_list_add_entry(list, text, NULL, NULL, false);
}

void morpthis_list_set_playing(morpthis_widget_t *list, int index) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		return;
	}
	if (index < 0 || index >= list->item_count) {
		index = -1;
	}
	list->playing_index = index;
	for (int i = 0; i < list->item_count; i++) {
		if (list->rows[i] && list->rows[i]->playmark) {
			lv_obj_set_hidden(list->rows[i]->playmark, i != index);
		}
	}
}

void morpthis_list_clear(morpthis_widget_t *list) {
	if (!list || list->kind != SDK_WIDGET_LIST) {
		return;
	}
	if (list->obj) {
		lv_obj_clean(list->obj);
		lv_obj_scroll_to_y(list->obj, 0, LV_ANIM_OFF);
	}
	for (int i = 0; i < list->item_count; i++) {
		sdk_list_row_t *row = list->rows[i];
		if (!row) {
			continue;
		}
		cover_free(&row->icon_cover);
		free(row);
	}
	free(list->rows);
	list->rows = NULL;
	list->row_capacity = 0;
	list->item_count = 0;
	list->playing_index = -1;
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

bool morpthis_list_scroll_to(morpthis_widget_t *list, int index) {
	if (!list || list->kind != SDK_WIDGET_LIST || !list->rows || index < 0 || index >= list->item_count) {
		return false;
	}
	sdk_list_row_t *row = list->rows[index];
	if (!row || !row->row) {
		return false;
	}
	lv_obj_scroll_to_view(row->row, LV_ANIM_OFF);
	return true;
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

// One popover exists at a time, so the callback that its items run can live
// here rather than in a per-menu allocation. A new show replaces it, and the
// app teardown calls morpthis_menu_close() before the shared object goes away.
static morpthis_menu_cb sdk_menu_cb;
static void *sdk_menu_user;

static void sdk_menu_item_cb(void *user) {
	int index = (int)(intptr_t)user;
	morpthis_menu_cb cb = sdk_menu_cb;
	void *cb_user = sdk_menu_user;
	sdk_menu_cb = NULL;
	sdk_menu_user = NULL;
	if (cb) {
		cb(index, cb_user);
	}
}

static bool sdk_menu_show_at(lv_obj_t *anchor, const morpthis_menu_item_t *items, int count,
							 morpthis_menu_cb cb, void *user) {
	if (!anchor || !items || count <= 0 || !sdk_app || !sdk_app->alive) {
		return false;
	}
	if (count > MORPTHIS_MENU_MAX) {
		count = MORPTHIS_MENU_MAX;
	}

	popover_item_t rows[MORPTHIS_MENU_MAX];
	for (int i = 0; i < count; i++) {
		rows[i].label = items[i].label ? items[i].label : "";
		rows[i].action = sdk_menu_item_cb;
		rows[i].user = (void *)(intptr_t)i;
		rows[i].checked = items[i].checked;
	}

	sdk_menu_cb = cb;
	sdk_menu_user = user;
	popover_show(anchor, rows, count);
	return true;
}

bool morpthis_menu_show(morpthis_widget_t *anchor, const morpthis_menu_item_t *items, int count,
						morpthis_menu_cb cb, void *user) {
	if (!anchor || !anchor->obj) {
		return false;
	}
	return sdk_menu_show_at(anchor->obj, items, count, cb, user);
}

bool morpthis_menu_show_row(morpthis_widget_t *list, int row, const morpthis_menu_item_t *items,
							int count, morpthis_menu_cb cb, void *user) {
	if (!list || list->kind != SDK_WIDGET_LIST || !list->rows || row < 0 || row >= list->item_count) {
		return false;
	}
	sdk_list_row_t *entry = list->rows[row];
	if (!entry || !entry->row) {
		return false;
	}
	return sdk_menu_show_at(entry->row, items, count, cb, user);
}

void morpthis_menu_close(void) {
	sdk_menu_cb = NULL;
	sdk_menu_user = NULL;
	popover_close();
}

static void image_reload(morpthis_widget_t *widget) {
	if (!widget || widget->kind != SDK_WIDGET_IMAGE || !widget->image) {
		return;
	}

	lv_image_set_src(widget->obj, NULL);
	cover_free(&widget->cover);

	char path[APPS_FOLDER_MAX + 256];
	if (!morpthis_fs_resolve(widget->image, path, sizeof(path))) {
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

void morpthis_image_set_source(morpthis_widget_t *image, const char *relative_path) {
	if (!image || image->kind != SDK_WIDGET_IMAGE) {
		return;
	}
	char *copy = sdk_strdup(relative_path);
	if (!copy) {
		return;
	}
	free(image->image);
	image->image = copy;
	image_reload(image);
}

morpthis_widget_t *morpthis_qrcode_create(morpthis_screen_t *screen, int size) {
	if (size <= 0) {
		errno = EINVAL;
		return NULL;
	}
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_QRCODE);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_qrcode_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	lv_qrcode_set_size(widget->obj, size);
	lv_qrcode_set_dark_color(widget->obj, theme()->text_primary);
	lv_qrcode_set_light_color(widget->obj, theme()->surface);
	lv_obj_set_size(widget->obj, size, size);
	return widget;
}

void morpthis_qrcode_set_text(morpthis_widget_t *qrcode, const char *text) {
	if (!qrcode || qrcode->kind != SDK_WIDGET_QRCODE || !qrcode->obj) {
		return;
	}
	char *copy = sdk_strdup(text);
	if (!copy) {
		return;
	}
	free(qrcode->text);
	qrcode->text = copy;
	lv_qrcode_set_data(qrcode->obj, qrcode->text);
}

static void roller_changed_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || widget->kind != SDK_WIDGET_ROLLER || !widget->obj) {
		return;
	}
	widget->value = lv_roller_get_selected(widget->obj);
	if (widget->value_cb) {
		widget->value_cb(widget, widget->value, widget->value_user);
	}
}

morpthis_widget_t *morpthis_roller_create(morpthis_screen_t *screen, const char *const *options,
										  int count, int selected, int visible_rows,
										  morpthis_value_cb cb, void *user) {
	if (!options || count <= 0) {
		return NULL;
	}
	morpthis_widget_t *widget = widget_new(screen, SDK_WIDGET_ROLLER);
	if (!widget) {
		return NULL;
	}
	widget->obj = lv_roller_create(screen->content);
	if (!widget->obj) {
		morpthis_widget_destroy(widget);
		return NULL;
	}

	size_t length = 0;
	for (int i = 0; i < count; i++) {
		length += strlen(options[i] ? options[i] : "") + 1;
	}
	char *joined = malloc(length + 1);
	if (!joined) {
		morpthis_widget_destroy(widget);
		return NULL;
	}
	joined[0] = '\0';
	for (int i = 0; i < count; i++) {
		if (i) {
			strcat(joined, "\n");
		}
		strcat(joined, options[i] ? options[i] : "");
	}
	lv_roller_set_options(widget->obj, joined, LV_ROLLER_MODE_NORMAL);
	free(joined);

	lv_obj_set_width(widget->obj, lv_pct(100));
	lv_roller_set_visible_row_count(widget->obj, visible_rows > 0 ? visible_rows : 3);
	lv_obj_set_style_text_font(widget->obj, sdk_ui_compact() ? &font_ui_18 : &font_ui_22, 0);
	lv_obj_set_style_bg_color(widget->obj, theme()->surface, 0);
	lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(widget->obj, theme()->text_primary, 0);
	lv_obj_set_style_border_width(widget->obj, 0, 0);

	widget->value_cb = cb;
	widget->value_user = user;
	widget->value = selected >= 0 && selected < count ? selected : 0;
	lv_roller_set_selected(widget->obj, widget->value, LV_ANIM_OFF);
	lv_obj_add_event_cb(widget->obj, roller_changed_cb, LV_EVENT_VALUE_CHANGED, widget);
	return widget;
}

void morpthis_roller_set_selected(morpthis_widget_t *roller, int index) {
	if (!roller || roller->kind != SDK_WIDGET_ROLLER || !roller->obj) {
		return;
	}
	if (index < 0) {
		index = 0;
	}
	lv_roller_set_selected(roller->obj, index, LV_ANIM_OFF);
	roller->value = lv_roller_get_selected(roller->obj);
}

int morpthis_roller_selected(const morpthis_widget_t *roller) {
	return roller && roller->kind == SDK_WIDGET_ROLLER ? roller->value : 0;
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

// The one event a seek bar acts on: the finger let go. PRESS_LOST is the same
// moment when the finger slides off the control, and without it a seek would
// stay "in progress" and the page would stop following the stream.
static void slider_release_event_cb(lv_event_t *e) {
	morpthis_widget_t *widget = lv_event_get_user_data(e);
	if (!widget || widget->kind != SDK_WIDGET_SLIDER || !widget->release_cb) {
		return;
	}
	widget->value = lv_slider_get_value(widget->obj);
	widget->release_cb(widget, widget->value, widget->release_user);
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
	lv_obj_add_event_cb(widget->obj, slider_release_event_cb, LV_EVENT_RELEASED, widget);
	lv_obj_add_event_cb(widget->obj, slider_release_event_cb, LV_EVENT_PRESS_LOST, widget);
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

void morpthis_slider_set_release_cb(morpthis_widget_t *slider, morpthis_value_cb cb, void *user) {
	if (!slider || slider->kind != SDK_WIDGET_SLIDER) {
		return;
	}
	slider->release_cb = cb;
	slider->release_user = user;
}

void morpthis_slider_set_range(morpthis_widget_t *slider, int min, int max) {
	if (!slider || slider->kind != SDK_WIDGET_SLIDER || !slider->obj) {
		return;
	}
	if (max < min) {
		int swap = min;
		min = max;
		max = swap;
	}
	slider->min = min;
	slider->max = max;
	lv_slider_set_range(slider->obj, min, max);
	int value = slider->value;
	if (value < min) {
		value = min;
	}
	if (value > max) {
		value = max;
	}
	// A range change is not a user edit: clamp the value without firing the
	// value callback.
	slider->setting_value = true;
	lv_slider_set_value(slider->obj, value, LV_ANIM_OFF);
	slider->setting_value = false;
	slider->value = value;
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

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

struct morpthis_timer {
	lv_timer_t *timer;
	morpthis_timer_cb cb;
	void *user;
	struct morpthis_timer *next;
};

static morpthis_timer_t *sdk_timers;

static void sdk_timer_tick(lv_timer_t *timer) {
	morpthis_timer_t *handle = lv_timer_get_user_data(timer);
	if (handle && handle->cb) {
		handle->cb(handle, handle->user);
	}
}

morpthis_timer_t *morpthis_timer_create(uint32_t period_ms, morpthis_timer_cb cb, void *user) {
	if (!cb || !sdk_app || !sdk_app->alive) {
		return NULL;
	}
	if (period_ms == 0) {
		period_ms = 1;
	}
	morpthis_timer_t *handle = calloc(1, sizeof(*handle));
	if (!handle) {
		return NULL;
	}
	handle->cb = cb;
	handle->user = user;
	handle->timer = lv_timer_create(sdk_timer_tick, period_ms, handle);
	if (!handle->timer) {
		free(handle);
		return NULL;
	}
	handle->next = sdk_timers;
	sdk_timers = handle;
	return handle;
}

void morpthis_timer_set_period(morpthis_timer_t *timer, uint32_t period_ms) {
	if (!timer || !timer->timer) {
		return;
	}
	lv_timer_set_period(timer->timer, period_ms ? period_ms : 1);
}

void morpthis_timer_reset(morpthis_timer_t *timer) {
	if (timer && timer->timer) {
		lv_timer_reset(timer->timer);
	}
}

void morpthis_timer_delete(morpthis_timer_t *timer) {
	if (!timer) {
		return;
	}
	morpthis_timer_t **link = &sdk_timers;
	while (*link && *link != timer) {
		link = &(*link)->next;
	}
	if (*link == timer) {
		*link = timer->next;
	}
	if (timer->timer) {
		lv_timer_delete(timer->timer);
	}
	free(timer);
}

void morpthis_widget_destroy(morpthis_widget_t *widget) {
	if (!widget) {
		return;
	}

	// A prompt is a modal owned by the SDK; if the field that opened it is going
	// away, close the prompt first so its callback cannot touch freed memory.
	if (prompt_owner == widget) {
		prompt_abandon();
	}

	// Children first: each unlinks itself from this widget's child list.
	while (widget->children) {
		morpthis_widget_destroy(widget->children);
	}

	if (widget->parent) {
		morpthis_widget_t **link = &widget->parent->children;
		while (*link && *link != widget) {
			link = &(*link)->next_sibling;
		}
		if (*link == widget) {
			*link = widget->next_sibling;
		}
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
		if (widget->kind == SDK_WIDGET_LIST) {
			// Deletes the row objects and frees the per-row covers and structs.
			morpthis_list_clear(widget);
		} else if (widget->kind == SDK_WIDGET_IMAGE || widget->kind == SDK_WIDGET_CANVAS) {
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
	if (!widget) {
		return;
	}
	if (widget->kind == SDK_WIDGET_TEXT_INPUT) {
		morpthis_text_input_set_text(widget, text);
		return;
	}
	if (widget->kind != SDK_WIDGET_LABEL && widget->kind != SDK_WIDGET_BUTTON &&
		widget->kind != SDK_WIDGET_SETTINGS_ROW) {
		return;
	}
	char *copy = sdk_strdup(text);
	if (!copy) {
		return;
	}
	free(widget->text);
	widget->text = copy;
	if (widget->label) {
		if (widget->text_scroll) {
			scrolltext_set(widget->label, widget->text);
		} else {
			lv_label_set_text(widget->label, widget->text);
		}
	}
}

const char *morpthis_widget_text(const morpthis_widget_t *widget) {
	if (!widget) {
		return "";
	}
	if (widget->kind == SDK_WIDGET_TEXT_INPUT && widget->obj) {
		return lv_textarea_get_text(widget->obj);
	}
	return widget->text ? widget->text : "";
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
	widget->width = width == MORPTHIS_SIZE_CONTENT || width < 0 ? 0 : width;
	widget->height = height == MORPTHIS_SIZE_CONTENT || height < 0 ? 0 : height;
	lv_obj_set_size(widget->obj, width == MORPTHIS_SIZE_CONTENT ? LV_SIZE_CONTENT : width,
					height == MORPTHIS_SIZE_CONTENT ? LV_SIZE_CONTENT : height);
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

void morpthis_widget_set_parent(morpthis_widget_t *widget, morpthis_widget_t *parent) {
	if (!widget || !widget->obj || widget == parent) {
		return;
	}
	if (parent && parent->kind != SDK_WIDGET_CONTAINER) {
		return;
	}
	if (widget->parent) {
		morpthis_widget_t **link = &widget->parent->children;
		while (*link && *link != widget) {
			link = &(*link)->next_sibling;
		}
		if (*link == widget) {
			*link = widget->next_sibling;
		}
	}
	widget->parent = parent;
	widget->next_sibling = NULL;
	if (parent) {
		widget->next_sibling = parent->children;
		parent->children = widget;
		lv_obj_set_parent(widget->obj, parent->obj);
	} else if (widget->screen) {
		lv_obj_set_parent(widget->obj, widget->screen->content);
	}
}

static lv_color_t rgb_color(uint32_t rgb) {
	return lv_color_make((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

void morpthis_widget_set_bg_color(morpthis_widget_t *widget, uint32_t rgb, int opacity) {
	if (!widget || !widget->obj) {
		return;
	}
	lv_obj_set_style_bg_color(widget->obj, rgb_color(rgb), 0);
	if (opacity >= 0) {
		if (opacity > 100) {
			opacity = 100;
		}
		lv_obj_set_style_bg_opa(widget->obj, (lv_opa_t)((opacity * 255 + 50) / 100), 0);
	}
}

void morpthis_widget_set_text_color(morpthis_widget_t *widget, uint32_t rgb) {
	if (!widget || !widget->obj) {
		return;
	}
	lv_obj_set_style_text_color(widget->obj, rgb_color(rgb), 0);
	if (widget->label && widget->label != widget->obj) {
		lv_obj_set_style_text_color(widget->label, rgb_color(rgb), 0);
	}
	if (widget->value_label) {
		lv_obj_set_style_text_color(widget->value_label, rgb_color(rgb), 0);
	}
}

void morpthis_widget_set_radius(morpthis_widget_t *widget, int radius) {
	if (!widget || !widget->obj) {
		return;
	}
	lv_obj_set_style_radius(widget->obj, radius < 0 ? 0 : radius, 0);
}

void morpthis_widget_set_padding(morpthis_widget_t *widget, int left, int top, int right, int bottom) {
	if (!widget || !widget->obj) {
		return;
	}
	lv_obj_set_style_pad_left(widget->obj, left < 0 ? 0 : left, 0);
	lv_obj_set_style_pad_top(widget->obj, top < 0 ? 0 : top, 0);
	lv_obj_set_style_pad_right(widget->obj, right < 0 ? 0 : right, 0);
	lv_obj_set_style_pad_bottom(widget->obj, bottom < 0 ? 0 : bottom, 0);
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

void morpthis_toast_success(const char *text) { toast_success(text ? text : ""); }

void morpthis_toast_error(const char *text) { toast_error(text ? text : ""); }

void morpthis_toast_busy(const char *text) { toast_busy(text ? text : ""); }

void morpthis_toast_busy_end(void) { toast_busy_end(); }

static bool sdk_modal_open;

void morpthis_ui_modal_show(const char *title, const char *subtitle) {
	if (!sdk_app || !sdk_app->alive) {
		return;
	}
	sdk_modal_open = true;
	gui_modal_show(NULL, theme()->accent, title ? title : "", subtitle ? subtitle : "");
}

void morpthis_ui_modal_progress(int percent) {
	if (sdk_modal_open) {
		gui_modal_progress(percent);
	}
}

void morpthis_ui_modal_hide(void) {
	if (sdk_modal_open) {
		sdk_modal_open = false;
		gui_modal_hide();
	}
}

// ---------------------------------------------------------------------------
// Theme and chrome
// ---------------------------------------------------------------------------

static uint32_t theme_rgb(lv_color_t color) {
	return ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) | (uint32_t)color.blue;
}

void morpthis_ui_theme(morpthis_theme_t *out) {
	if (!out) {
		return;
	}
	const theme_palette_t *palette = theme();
	out->screen_bg = theme_rgb(palette->screen_bg);
	out->surface = theme_rgb(palette->surface);
	out->panel = theme_rgb(palette->panel);
	out->cover_bg = theme_rgb(palette->cover_bg);
	out->text_primary = theme_rgb(palette->text_primary);
	out->text_secondary = theme_rgb(palette->text_secondary);
	out->accent = theme_rgb(palette->accent);
	out->dark = palette->dark;
}

bool morpthis_ui_set_dark(bool dark) {
	theme_set_dark(dark);
	return true;
}

bool morpthis_ui_set_accent(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		return false;
	}
	theme_set_accent(index);
	return true;
}

void morpthis_ui_set_status_bar_visible(bool visible) {
	if (app_is_game()) {
		return; // the game owns the whole panel; the player's chrome stays away
	}
	topbar_set_hidden(!visible);
}

bool morpthis_ui_status_bar_visible(void) {
	return !topbar_is_hidden();
}

// ---------------------------------------------------------------------------
// Modal dialogs
// ---------------------------------------------------------------------------

static morpthis_action_cb sdk_confirm_cb;
static void *sdk_confirm_user;
static bool sdk_confirm_open;

static void sdk_confirm_action(void *user) {
	(void)user;
	morpthis_action_cb cb = sdk_confirm_cb;
	void *cb_user = sdk_confirm_user;
	sdk_confirm_open = false;
	sdk_confirm_cb = NULL;
	sdk_confirm_user = NULL;
	if (cb) {
		cb(cb_user);
	}
}

bool morpthis_ui_confirm(const char *title, const char *message, const char *ok_label,
					 morpthis_action_cb cb, void *user) {
	if (!sdk_app || !sdk_app->alive) {
		return false;
	}
	sdk_confirm_cb = cb;
	sdk_confirm_user = user;
	sdk_confirm_open = true;
	confirm_show(title ? title : "", message, ok_label, sdk_confirm_action, NULL);
	return true;
}

void morpthis_ui_notice(const char *title, const char *message) {
	if (!sdk_app || !sdk_app->alive) {
		return;
	}
	// A notice has no callback into the app, so it needs no teardown tracking:
	// the player owns the card and can close it whenever it likes.
	confirm_notice(title ? title : "", message);
}

// ---------------------------------------------------------------------------
// Text prompt
// ---------------------------------------------------------------------------

static lv_obj_t *prompt_modal;
static lv_obj_t *prompt_field;
static keyboard_t *prompt_keyboard;
static morpthis_text_cb prompt_cb;
static void *prompt_user;

static void prompt_finish(bool deliver, bool accepted);

static void prompt_cancel_cb(lv_event_t *e) {
	(void)e;
	prompt_finish(true, false);
}

static void prompt_accept_cb(lv_event_t *e) {
	(void)e;
	prompt_finish(true, true);
}

// Destroys the card without entering the app. Used by teardown: a callback into
// a shared object that is about to be unmapped would be the last thing it ever
// did.
static void prompt_abandon(void) { prompt_finish(false, false); }

static void prompt_finish(bool deliver, bool accepted) {
	if (!prompt_modal) {
		return;
	}

	char *text = NULL;
	if (accepted && prompt_field) {
		const char *value = lv_textarea_get_text(prompt_field);
		if (value) {
			text = sdk_strdup(value);
		}
	}

	morpthis_text_cb cb = prompt_cb;
	void *cb_user = prompt_user;
	keyboard_t *kb = prompt_keyboard;
	lv_obj_t *modal = prompt_modal;

	prompt_modal = NULL;
	prompt_field = NULL;
	prompt_keyboard = NULL;
	prompt_cb = NULL;
	prompt_user = NULL;
	prompt_owner = NULL;

	keyboard_destroy(kb);
	lv_obj_delete(modal);

	if (deliver && cb) {
		cb(accepted, text ? text : "", cb_user);
	}
	free(text);
}

static bool prompt_show(const char *title, const char *initial, bool password, morpthis_text_cb cb,
						void *user, morpthis_widget_t *owner) {
	if (!sdk_cfg || !sdk_app || !sdk_app->alive || !cb) {
		return false;
	}

	// A second prompt replaces the first; the first is cancelled without a
	// callback, because the app is already busy opening another.
	prompt_abandon();

	lv_obj_t *modal = lv_obj_create(lv_layer_top());
	if (!modal) {
		return false;
	}
	lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(modal, LV_OPA_60, 0);
	lv_obj_set_style_border_width(modal, 0, 0);
	lv_obj_set_style_pad_all(modal, 0, 0);
	lv_obj_set_scrollable(modal, false);
	lv_obj_set_clickable(modal, true);

	lv_obj_t *card = lv_obj_create(modal);
	lv_obj_set_width(card, lv_pct(90));
	lv_obj_set_height(card, 110);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 14);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 14, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 10, 0);
	lv_obj_set_scrollable(card, false);

	lv_obj_t *heading = lv_label_create(card);
	lv_label_set_text(heading, title ? title : "");
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_18, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *field = lv_textarea_create(card);
	lv_obj_set_width(field, lv_pct(100));
	lv_obj_set_height(field, 36);
	lv_obj_align(field, LV_ALIGN_TOP_MID, 0, 24);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_text(field, initial ? initial : "");
	lv_obj_set_style_bg_color(field, theme()->surface, 0);
	lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(field, theme()->text_primary, 0);
	lv_obj_set_style_text_font(field, &font_ui_16, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_radius(field, 8, 0);
	lv_obj_set_style_pad_all(field, 6, 0);
	if (password) {
		lv_textarea_set_password_mode(field, true);
		keyboard_style_password(field, 0);
	}
	lv_obj_add_state(field, LV_STATE_FOCUSED);

	lv_obj_t *cancel = lv_button_create(card);
	lv_obj_set_size(cancel, 74, 28);
	lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_t *cancel_label = lv_label_create(cancel);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_add_style(cancel_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(cancel_label, &font_ui_14, 0);
	lv_obj_center(cancel_label);
	lv_obj_add_event_cb(cancel, prompt_cancel_cb, LV_EVENT_CLICKED, NULL);

	int keyboard_height = sdk_cfg->screen_width < 320 ? 144 : 316;
	keyboard_t *kb = keyboard_create(modal, sdk_cfg->screen_width, keyboard_height, field, NULL, "OK",
								 prompt_accept_cb, NULL);
	if (!kb) {
		lv_obj_delete(modal);
		return false;
	}

	prompt_modal = modal;
	prompt_field = field;
	prompt_keyboard = kb;
	prompt_cb = cb;
	prompt_user = user;
	prompt_owner = owner;
	lv_obj_move_foreground(modal);
	return true;
}

bool morpthis_ui_prompt_text(const char *title, const char *initial, bool password,
						 morpthis_text_cb cb, void *user) {
	return prompt_show(title, initial, password, cb, user, NULL);
}

// Called from the host's teardown, on the GUI thread. Removes every piece of
// player chrome that holds a callback into the app, before the shared object is
// unmapped.
void sdk_ui_app_reset(bool teardown) {
	morpthis_menu_close();
	prompt_abandon();
	if (sdk_confirm_open) {
		confirm_close();
	}
	sdk_confirm_open = false;
	sdk_confirm_cb = NULL;
	sdk_confirm_user = NULL;
	morpthis_ui_modal_hide();

	// The app's own timers belong to its loaded lifetime, not to a screen. A
	// backgrounded app keeps them (a music app may poll while away); a teardown
	// must delete them before the shared object is unmapped.
	if (teardown) {
		while (sdk_timers) {
			morpthis_timer_delete(sdk_timers);
		}
	}

	// An app that hid the status bar must not leave the next page wearing that
	// choice. A game's own screen hooks put the bar back on unload.
	if (!app_is_game()) {
		topbar_set_hidden(false);
	}
}
