#include "pictureview.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

lv_obj_t *pictureview_screen;

static gui_config_t *view_cfg;
static lv_obj_t *title_label;
static lv_obj_t *viewport;
static lv_obj_t *picture_image;
static cover_image_t held_image;
static bool has_image;

bool pictureview_open(const char *path) {
	if (!path || !path[0] || !view_cfg || !picture_image) {
		return false;
	}

	int top = settingsrow_content_top(view_cfg);
	int box_w = view_cfg->screen_width - 2 * view_cfg->padding;
	int box_h = view_cfg->screen_height - top - view_cfg->padding;
	if (box_w < 1) {
		box_w = 1;
	}
	if (box_h < 1) {
		box_h = 1;
	}

	cover_image_t decoded;
	memset(&decoded, 0, sizeof(decoded));
	if (!cover_load_image_file(path, box_w, box_h, COVER_FIT_CONTAIN, &decoded)) {
		return false;
	}

	if (has_image) {
		lv_image_set_src(picture_image, NULL);
		cover_free(&held_image);
		has_image = false;
	}

	held_image = decoded;
	has_image = true;

	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);

	lv_obj_set_size(picture_image, (int32_t)held_image.dsc.header.w, (int32_t)held_image.dsc.header.h);
	lv_image_set_src(picture_image, &held_image.dsc);
	lv_obj_center(picture_image);

	switch_screen(pictureview_screen);
	return true;
}

void pictureview_init(gui_config_t *cfg) {
	view_cfg = cfg;
	lv_obj_add_style(pictureview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(pictureview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int top = settingsrow_content_top(cfg);
	viewport = lv_obj_create(pictureview_screen);
	lv_obj_remove_style_all(viewport);
	lv_obj_set_size(viewport, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - top - cfg->padding);
	lv_obj_align(viewport, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_scrollable(viewport, false);
	lv_obj_set_clickable(viewport, true);
	lv_obj_set_event_bubble(viewport, true);

	picture_image = lv_image_create(viewport);
	lv_image_set_inner_align(picture_image, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_clickable(picture_image, false);
	lv_obj_set_scrollable(picture_image, false);
	lv_obj_center(picture_image);

	player_sheet_attach_drag(viewport, true);
	switcher_attach_back_gesture(pictureview_screen);
}
