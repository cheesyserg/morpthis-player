#include "bootscreen.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/system.h"
#include "src/system/image/art_shrink.h"

lv_obj_t *bootscreen_screen;

// The folder the user drops pictures into, at the root of the card. The name
// starts with a dot so it stays out of the file browser's way, the same as
// .local and the other folders this player keeps there.
#define BOOTSCREEN_DIR ".bootscreen"

// A card holds a handful of pictures, not a library. The cap is what keeps the
// page's memory bounded when somebody drops a folder of wallpapers in.
#define BOOTSCREEN_MAX 64
#define BOOTSCREEN_NAME_MAX 128
#define BOOTSCREEN_PATH_MAX 512

// How much smaller a picture at the edge of the carousel is drawn than the one
// in the middle. 0.55 leaves the neighbours clearly present without competing
// with the selection.
#define CAROUSEL_EDGE_SCALE 0.55f

// The full-screen preview's grow and shrink. The overshoot on the way in is
// what makes the picture feel like it pops out of the carousel rather than
// simply sliding to the middle of the panel.
#define PREVIEW_OPEN_MS  320
#define PREVIEW_CLOSE_MS 260

typedef enum {
	ENTRY_APPLIED,
	ENTRY_STOCK,
	ENTRY_RETROSPACE,
	ENTRY_CUSTOM,
} entry_kind_t;

typedef struct {
	entry_kind_t kind;
	char name[BOOTSCREEN_NAME_MAX]; // the file name, or "Stock"/"Retrospace"
	char path[BOOTSCREEN_PATH_MAX];
	cover_image_t art; // decoded at the carousel's large size
	bool art_loaded;
} entry_t;

static gui_config_t *page_cfg;
static entry_t entries[BOOTSCREEN_MAX];
static int entry_count;
static int selected = -1;
static int applied_entry = -1;

static lv_obj_t *carousel;
static lv_obj_t *carousel_separator;
static lv_obj_t *item_btn[BOOTSCREEN_MAX];
static lv_obj_t *item_img[BOOTSCREEN_MAX];
static lv_obj_t *applied_check;
static lv_obj_t *apply_btn;
static lv_obj_t *apply_label;
static bool carousel_rebuilding;
static bool apply_running;

static lv_obj_t *preview_layer;
static lv_obj_t *preview_img;
static cover_image_t preview_art;
static bool preview_art_loaded;
static bool preview_open;
static bool preview_animating;
static bool preview_closing;
static int preview_start_x;
static int preview_start_y;
static uint32_t preview_start_scale;

static int item_w;
static int item_h;
static int carousel_w;

// Draws one of the interface's glyphs at `size`, whatever its source art is.
// The check on the applied picture and on its carousel item are the two users.
static void scale_icon(lv_obj_t *icon, const lv_image_dsc_t *source, int size) {
	lv_obj_set_size(icon, size, size);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	int source_side = LV_MAX((int)source->header.w, (int)source->header.h);
	if (source_side > 0) {
		lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * size / source_side));
	}
}

// ---------------------------------------------------------------------------
// The full-screen preview
//
// A tap on the picture in the middle lifts it out of the carousel and grows it
// to fill the panel, over a black sheet that covers the page, the status bar
// and the chevron. A second tap shrinks it back into the row. The animation
// moves the picture's centre and its scale together, so it reads as one object
// growing rather than a second picture fading in.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *img;
	int32_t start_x, start_y, end_x, end_y;
	uint32_t start_scale, end_scale;
} preview_anim_t;

static preview_anim_t preview_anim_data;

static void preview_anim_cb(void *var, int32_t value) {
	preview_anim_t *a = var;
	int32_t x = a->start_x + (a->end_x - a->start_x) * value / 1000;
	int32_t y = a->start_y + (a->end_y - a->start_y) * value / 1000;
	int32_t scale = (int32_t)a->start_scale + ((int32_t)a->end_scale - (int32_t)a->start_scale) * value / 1000;

	lv_obj_set_x(a->img, x);
	lv_obj_set_y(a->img, y);
	lv_obj_set_style_transform_scale(a->img, scale, 0);
}

static void preview_anim_done_cb(lv_anim_t *a) {
	(void)a;
	preview_animating = false;

	if (preview_closing) {
		preview_closing = false;
		preview_open = false;
		lv_obj_set_hidden(preview_layer, true);
		topbar_set_hidden(false);
		back_btn_force_hidden(false);
	}
}

static void preview_close_cb(lv_event_t *e);

static void preview_open_cb(lv_event_t *e) {
	(void)e;
	if (preview_open || preview_animating) {
		return;
	}
	if (selected < 0 || selected >= entry_count || !item_img[selected]) {
		return;
	}

	entry_t *entry = &entries[selected];
	if (preview_art_loaded) {
		lv_image_set_src(preview_img, NULL);
		cover_free(&preview_art);
		preview_art_loaded = false;
	}
	if (!cover_load_image_file(entry->path, (int)page_cfg->screen_width, (int)page_cfg->screen_height,
							   COVER_FIT_COVER, &preview_art)) {
		return;
	}
	preview_art_loaded = true;

	// Where the picture is on screen right now, and how big it is drawn.
	lv_obj_t *src = item_img[selected];
	lv_area_t area;
	lv_obj_get_coords(src, &area);
	int cx = (area.x1 + area.x2) / 2;
	int cy = (area.y1 + area.y2) / 2;

	int src_w = lv_obj_get_width(src) * lv_image_get_scale(src) / LV_SCALE_NONE;
	if (src_w < 1) {
		src_w = 1;
	}
	uint32_t start_scale = (uint32_t)((int64_t)LV_SCALE_NONE * src_w / (int)page_cfg->screen_width);
	if (start_scale < 1) {
		start_scale = 1;
	}

	preview_start_x = cx - (int)page_cfg->screen_width / 2;
	preview_start_y = cy - (int)page_cfg->screen_height / 2;
	preview_start_scale = start_scale;

	lv_obj_set_size(preview_img, (int32_t)page_cfg->screen_width, (int32_t)page_cfg->screen_height);
	lv_image_set_src(preview_img, &preview_art.dsc);
	lv_obj_set_x(preview_img, preview_start_x);
	lv_obj_set_y(preview_img, preview_start_y);
	lv_obj_set_style_transform_scale(preview_img, (int32_t)preview_start_scale, 0);

	lv_obj_set_hidden(preview_layer, false);
	lv_obj_move_foreground(preview_layer);
	topbar_set_hidden(true);
	back_btn_force_hidden(true);

	preview_open = true;
	preview_animating = true;
	preview_closing = false;

	preview_anim_data.img = preview_img;
	preview_anim_data.start_x = preview_start_x;
	preview_anim_data.start_y = preview_start_y;
	preview_anim_data.end_x = 0;
	preview_anim_data.end_y = 0;
	preview_anim_data.start_scale = preview_start_scale;
	preview_anim_data.end_scale = LV_SCALE_NONE;

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &preview_anim_data);
	lv_anim_set_exec_cb(&a, preview_anim_cb);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, PREVIEW_OPEN_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
	lv_anim_set_completed_cb(&a, preview_anim_done_cb);
	lv_anim_start(&a);
}

static void preview_close_cb(lv_event_t *e) {
	(void)e;
	if (!preview_open || preview_animating) {
		return;
	}

	preview_animating = true;
	preview_closing = true;

	preview_anim_data.img = preview_img;
	preview_anim_data.start_x = lv_obj_get_x(preview_img);
	preview_anim_data.start_y = lv_obj_get_y(preview_img);
	preview_anim_data.end_x = preview_start_x;
	preview_anim_data.end_y = preview_start_y;
	preview_anim_data.start_scale = (uint32_t)lv_obj_get_style_transform_scale_x(preview_img, 0);
	preview_anim_data.end_scale = preview_start_scale;

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &preview_anim_data);
	lv_anim_set_exec_cb(&a, preview_anim_cb);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, PREVIEW_CLOSE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
	lv_anim_set_completed_cb(&a, preview_anim_done_cb);
	lv_anim_start(&a);
}

// The swipe-back and the chevron close the preview rather than leaving the
// page while it is up. The chevron itself is hidden, but the gesture is not.
static bool preview_back_guard(void) {
	if (!preview_open) {
		return false;
	}
	preview_close_cb(NULL);
	return true;
}

static bool preview_back_guard_peek(void) { return preview_open; }

// ---------------------------------------------------------------------------
// The applied picture
// ---------------------------------------------------------------------------

// The choice the device will really draw. A custom choice whose picture has
// gone -- a factory reset, a failed write -- falls back to Retrospace in the
// boot logo writer, so the page has to say the same thing.
static int effective_choice(void) {
	int choice = theme_boot_screen();
	if (choice == BOOTLOGO_CUSTOM && !bootlogo_custom_available()) {
		choice = BOOTLOGO_RETROSPACE;
	}
	return choice;
}

static const char *applied_path(void) {
	int choice = effective_choice();
	if (choice == BOOTLOGO_STOCK) {
		return theme()->dark ? "/etc/logo2.jpeg" : "/etc/logo1.jpeg";
	}
	if (choice == BOOTLOGO_RETROSPACE) {
		return "/etc/logo.jpeg";
	}
	return bootlogo_custom_path();
}

const char *bootscreen_applied_name(void) {
	static char name[BOOTSCREEN_NAME_MAX];
	int choice = effective_choice();

	if (choice == BOOTLOGO_STOCK) {
		snprintf(name, sizeof(name), "Stock");
	} else if (choice == BOOTLOGO_RETROSPACE) {
		snprintf(name, sizeof(name), "Retrospace");
	} else {
		const char *stored = config_get("ui", "boot_screen_name", "");
		snprintf(name, sizeof(name), "%s", stored[0] ? stored : tr("bootscreen_custom"));
	}
	return name;
}

static bool entry_is_applied(const entry_t *entry) {
	if (entry->kind == ENTRY_APPLIED) {
		return true;
	}

	int choice = effective_choice();
	if (choice == BOOTLOGO_STOCK) {
		return entry->kind == ENTRY_STOCK;
	}
	if (choice == BOOTLOGO_RETROSPACE) {
		return entry->kind == ENTRY_RETROSPACE;
	}
	if (entry->kind != ENTRY_CUSTOM) {
		return false;
	}

	const char *stored = config_get("ui", "boot_screen_name", "");
	return stored[0] && strcmp(entry->name, stored) == 0;
}

// ---------------------------------------------------------------------------
// The gallery
// ---------------------------------------------------------------------------

static void free_entry(entry_t *entry) {
	if (entry->art_loaded) {
		cover_free(&entry->art);
		entry->art_loaded = false;
	}
}

static void clear_entries(void) {
	for (int i = 0; i < entry_count; i++) {
		free_entry(&entries[i]);
	}
	entry_count = 0;
	selected = -1;
	applied_entry = -1;
}

static bool image_file(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".png") == 0;
}

static void add_entry(entry_kind_t kind, const char *name, const char *path) {
	if (entry_count >= BOOTSCREEN_MAX) {
		return;
	}

	entry_t *entry = &entries[entry_count++];
	memset(entry, 0, sizeof(*entry));
	entry->kind = kind;
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	snprintf(entry->path, sizeof(entry->path), "%s", path);
}

static void scan_entries(void) {
	clear_entries();

	// The applied picture first, in its own slot at the far left. The vertical
	// rule after it separates it from the choices.
	add_entry(ENTRY_APPLIED, bootscreen_applied_name(), applied_path());
	applied_entry = 0;

	int choice = effective_choice();

	// The two pictures the firmware itself carries. Stock follows the theme,
	// so the preview is the one the current palette would draw. The one that
	// is already applied is left out: it is the first slot.
	if (choice != BOOTLOGO_STOCK) {
		add_entry(ENTRY_STOCK, "Stock", theme()->dark ? "/etc/logo2.jpeg" : "/etc/logo1.jpeg");
	}
	if (choice != BOOTLOGO_RETROSPACE) {
		add_entry(ENTRY_RETROSPACE, "Retrospace", "/etc/logo.jpeg");
	}

	const char *root = storage_sd_root();
	if (root && root[0]) {
		char dir[BOOTSCREEN_PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/" BOOTSCREEN_DIR, root);

		const char *stored = config_get("ui", "boot_screen_name", "");
		struct dirent **list = NULL;
		int n = scandir(dir, &list, NULL, alphasort);
		if (n > 0) {
			for (int i = 0; i < n; i++) {
				const char *name = list[i]->d_name;
				if (name[0] != '.' && image_file(name)) {
					// The applied custom picture is already the first slot.
					if (choice == BOOTLOGO_CUSTOM && stored[0] && strcmp(name, stored) == 0) {
						free(list[i]);
						continue;
					}

					char path[BOOTSCREEN_PATH_MAX];
					snprintf(path, sizeof(path), "%s/%s", dir, name);
					struct stat st;
					if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
						add_entry(ENTRY_CUSTOM, name, path);
					}
				}
				free(list[i]);
			}
			free(list);
		} else if (list) {
			free(list);
		}
	}

	selected = 0;
}

static void load_art(entry_t *entry) {
	if (entry->art_loaded) {
		return;
	}
	if (cover_load_image_file(entry->path, item_w, item_h, COVER_FIT_COVER, &entry->art)) {
		entry->art_loaded = true;
	}
}


static void refresh_apply_button(void) {
	bool applied = selected >= 0 && selected < entry_count && entry_is_applied(&entries[selected]);
	if (applied) {
		lv_label_set_text(apply_label, tr("bootscreen_applied"));
		lv_obj_set_style_bg_color(apply_btn, theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(apply_label, theme()->text_secondary, 0);
	} else {
		lv_label_set_text(apply_label, tr("bootscreen_apply"));
		lv_obj_set_style_bg_color(apply_btn, theme()->accent, 0);
		lv_obj_set_style_text_color(apply_label, theme_on_accent(), 0);
	}
}

// ---------------------------------------------------------------------------
// The carousel
// ---------------------------------------------------------------------------

// The item nearest the middle is the selection. Everything else is drawn
// smaller the further it sits from that middle, which is what makes the row
// read as a carousel rather than a list of thumbnails.
static void carousel_update(void) {
	if (!carousel || carousel_rebuilding) {
		return;
	}

	lv_area_t carousel_area;
	lv_obj_get_coords(carousel, &carousel_area);
	int carousel_center = (carousel_area.x1 + carousel_area.x2) / 2;

	for (int i = 0; i < entry_count; i++) {
		lv_obj_t *item = item_btn[i];
		if (!item) {
			continue;
		}

		lv_area_t item_area;
		lv_obj_get_coords(item, &item_area);
		int item_center = (item_area.x1 + item_area.x2) / 2;
		int dist = item_center - carousel_center;
		int abs_dist = dist < 0 ? -dist : dist;

		float t = (float)abs_dist / (float)item_w;
		if (t > 1.0f) {
			t = 1.0f;
		}
		float scale = 1.0f - (1.0f - CAROUSEL_EDGE_SCALE) * t;
		lv_image_set_scale(item_img[i], (uint32_t)(LV_SCALE_NONE * scale));

		// The neighbours fade a little as they shrink, so the middle picture
		// is the one the eye lands on.
		lv_opa_t opa = (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER - LV_OPA_60) * t);
		lv_obj_set_style_image_opa(item_img[i], opa, 0);
	}
}

// The entry nearest the middle. The selection is only moved when the row
// comes to rest, never while it is still moving: changing it mid-snap is what
// used to make the row settle between two pictures.
static int carousel_closest(void) {
	if (!carousel) {
		return -1;
	}

	lv_area_t carousel_area;
	lv_obj_get_coords(carousel, &carousel_area);
	int carousel_center = (carousel_area.x1 + carousel_area.x2) / 2;

	int best = -1;
	int best_dist = INT_MAX;
	for (int i = 0; i < entry_count; i++) {
		lv_obj_t *item = item_btn[i];
		if (!item) {
			continue;
		}

		lv_area_t item_area;
		lv_obj_get_coords(item, &item_area);
		int item_center = (item_area.x1 + item_area.x2) / 2;
		int dist = item_center - carousel_center;
		int abs_dist = dist < 0 ? -dist : dist;
		if (abs_dist < best_dist) {
			best_dist = abs_dist;
			best = i;
		}
	}
	return best;
}

static void carousel_scroll_cb(lv_event_t *e) {
	(void)e;
	if (carousel_rebuilding) {
		return;
	}
	carousel_update();
}

// Where the carousel has to be scrolled for `index` to sit in the middle.
// The delta is measured from the absolute coordinates rather than from the
// item's layout x: the row's own left padding is part of what puts the first
// picture in the middle, and leaving it out is what used to park the selection
// between two pictures.
static int carousel_target_for(int index) {
	if (index < 0 || index >= entry_count || !item_btn[index]) {
		return 0;
	}

	lv_area_t item_area;
	lv_area_t carousel_area;
	lv_obj_get_coords(item_btn[index], &item_area);
	lv_obj_get_coords(carousel, &carousel_area);

	int item_center = (item_area.x1 + item_area.x2) / 2;
	int carousel_center = (carousel_area.x1 + carousel_area.x2) / 2;
	int target = lv_obj_get_scroll_x(carousel) + (item_center - carousel_center);

	// Clamped to what the row can actually scroll: asking for a position past
	// the end would make the snap animation end somewhere else and the next
	// SCROLL_END would ask for the same impossible target again.
	int max_scroll = lv_obj_get_scroll_x(carousel) + lv_obj_get_scroll_right(carousel);
	if (max_scroll < 0) {
		max_scroll = 0;
	}
	if (target < 0) {
		target = 0;
	}
	if (target > max_scroll) {
		target = max_scroll;
	}
	return target;
}

static void carousel_scroll_end_cb(lv_event_t *e) {
	(void)e;
	if (carousel_rebuilding) {
		return;
	}

	int best = carousel_closest();
	if (best < 0) {
		return;
	}

	if (best != selected) {
		selected = best;
		refresh_apply_button();
	}

	int target = carousel_target_for(selected);
	if (target == lv_obj_get_scroll_x(carousel)) {
		return;
	}
	lv_obj_scroll_to_x(carousel, target, LV_ANIM_ON);
}

static void item_clicked_cb(lv_event_t *e) {
	if (carousel_rebuilding || switcher_back_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}

	// A tap on the picture already in the middle opens it full screen; a tap
	// on a neighbour brings that one to the middle first.
	if (index == selected) {
		preview_open_cb(NULL);
		return;
	}

	selected = index;
	refresh_apply_button();
	lv_obj_scroll_to_x(carousel, carousel_target_for(index), LV_ANIM_ON);
	carousel_update();
}

static void refresh_carousel(void) {
	// Rebuilding while a scroll animation is still running is what used to
	// freeze the page: the animation kept moving a row whose items had just
	// been deleted. Stop it first, and keep the scroll callback out until the
	// new row is laid out.
	carousel_rebuilding = true;
	lv_obj_scroll_to_x(carousel, lv_obj_get_scroll_x(carousel), LV_ANIM_OFF);
	lv_obj_clean(carousel);
	carousel_separator = NULL;
	applied_check = NULL;
	for (int i = 0; i < BOOTSCREEN_MAX; i++) {
		item_btn[i] = NULL;
		item_img[i] = NULL;
	}

	for (int i = 0; i < entry_count; i++) {
		entry_t *entry = &entries[i];
		load_art(entry);

		lv_obj_t *item = lv_btn_create(carousel);
		item_btn[i] = item;
		lv_obj_set_size(item, item_w, item_h);
		lv_obj_set_style_radius(item, 0, 0);
		lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(item, 0, 0);
		lv_obj_set_style_shadow_width(item, 0, 0);
		lv_obj_set_style_pad_all(item, 0, 0);
		lv_obj_set_scrollable(item, false);
		lv_obj_add_event_cb(item, item_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *img = lv_image_create(item);
		item_img[i] = img;
		lv_obj_set_size(img, item_w, item_h);
		lv_obj_set_clickable(img, false);
		if (entry->art_loaded) {
			lv_image_set_src(img, &entry->art.dsc);
		}
		lv_obj_center(img);

		if (i == applied_entry) {
			lv_obj_t *check = lv_image_create(item);
			lv_image_set_src(check, &icon_check);
			lv_obj_add_style(check, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor(check, theme()->accent, 0);
			lv_obj_set_style_image_recolor_opa(check, LV_OPA_COVER, 0);
			scale_icon(check, &icon_check, page_cfg->screen_width < 320 ? 16 : 22);
			lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -2, 2);
			applied_check = check;
		}

		// The rule between the applied picture and the choices. It is part of
		// the row, so it scrolls with them.
		if (i == 0) {
			carousel_separator = lv_obj_create(carousel);
			lv_obj_set_size(carousel_separator, 1, item_h);
			lv_obj_set_style_bg_color(carousel_separator, theme()->text_secondary, 0);
			lv_obj_set_style_bg_opa(carousel_separator, LV_OPA_20, 0);
			lv_obj_set_style_border_width(carousel_separator, 0, 0);
			lv_obj_set_style_radius(carousel_separator, 0, 0);
			lv_obj_set_style_pad_all(carousel_separator, 0, 0);
			lv_obj_set_scrollable(carousel_separator, false);
		}
	}

	lv_obj_update_layout(carousel);
	if (selected >= 0 && selected < entry_count) {
		lv_obj_scroll_to_x(carousel, carousel_target_for(selected), LV_ANIM_OFF);
	}
	carousel_rebuilding = false;
	carousel_update();
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

// Turns whatever the user put on the card into the JPEG the boot script can
// draw, at the panel's own size. Quality steps down until the result fits the
// flash block; a picture that cannot be made to fit is refused rather than
// written half-way.
static bool encode_custom(const char *path, uint8_t **out, size_t *out_len) {
	int w = 0, h = 0;
	uint8_t *rgb = cover_load_image_rgb(path, (int)page_cfg->screen_width, (int)page_cfg->screen_height,
										COVER_FIT_COVER, &w, &h);
	if (!rgb) {
		return false;
	}

	static const int qualities[] = {90, 80, 70, 60};
	for (size_t i = 0; i < sizeof(qualities) / sizeof(qualities[0]); i++) {
		size_t len = 0;
		uint8_t *jpeg = art_encode_jpeg_rgb(rgb, w, h, qualities[i], &len);
		if (jpeg && len > 0 && len <= BOOTLOGO_IMAGE_MAX) {
			free(rgb);
			*out = jpeg;
			*out_len = len;
			return true;
		}
		free(jpeg);
	}

	free(rgb);
	return false;
}

typedef struct {
	char path[BOOTSCREEN_PATH_MAX];
	char name[BOOTSCREEN_NAME_MAX];
	bool ok;
} apply_job_t;

static void apply_done_cb(void *user) {
	apply_job_t *job = user;
	apply_running = false;

	if (job->ok) {
		config_set("ui", "boot_screen_name", job->name);
		// Forced: applying the same picture again has to repair a block that
		// an older build may have written with the picture in the wrong place.
		theme_set_boot_screen_force(BOOTLOGO_CUSTOM);
		scan_entries();
		refresh_carousel();
		refresh_apply_button();
		toast_success("bootscreen_applied_toast");
	} else {
		toast_error("bootscreen_apply_failed");
	}

	free(job);
}

// Decoding, re-encoding and writing the picture is the slow part of Apply,
// and on this device it is slow enough to be seen. It runs here, off the UI
// thread, and the result comes back through gui_post().
static void *apply_worker(void *arg) {
	apply_job_t *job = arg;
	uint8_t *jpeg = NULL;
	size_t len = 0;

	if (encode_custom(job->path, &jpeg, &len)) {
		job->ok = bootlogo_custom_store(jpeg, len);
		free(jpeg);
	}

	if (!gui_post(apply_done_cb, job)) {
		free(job);
	}
	return NULL;
}

static void apply_cb(lv_event_t *e) {
	(void)e;
	if (carousel_rebuilding || apply_running || switcher_back_drag_active()) {
		return;
	}

	// The row may still be moving when Apply is tapped. Take the picture that
	// is in the middle right now, not the one that was there when the drag
	// started.
	int best = carousel_closest();
	if (best >= 0 && best != selected) {
		selected = best;
		refresh_apply_button();
	}
	if (selected < 0 || selected >= entry_count) {
		return;
	}

	entry_t *entry = &entries[selected];
	if (entry->kind == ENTRY_APPLIED || entry_is_applied(entry)) {
		return;
	}

	if (entry->kind == ENTRY_STOCK) {
		config_set("ui", "boot_screen_name", "");
		theme_set_boot_screen(BOOTLOGO_STOCK);
		scan_entries();
		refresh_carousel();
		refresh_apply_button();
		toast_success("bootscreen_applied_toast");
		return;
	}

	if (entry->kind == ENTRY_RETROSPACE) {
		config_set("ui", "boot_screen_name", "");
		theme_set_boot_screen(BOOTLOGO_RETROSPACE);
		scan_entries();
		refresh_carousel();
		refresh_apply_button();
		toast_success("bootscreen_applied_toast");
		return;
	}

	apply_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("bootscreen_apply_failed");
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", entry->path);
	snprintf(job->name, sizeof(job->name), "%s", entry->name);

	apply_running = true;
	toast_busy("bootscreen_applying");

	pthread_t thread;
	if (pthread_create(&thread, NULL, apply_worker, job) != 0) {
		apply_running = false;
		free(job);
		toast_error("bootscreen_apply_failed");
		return;
	}
	pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

static void repaint_cb(void) {
	if (!apply_btn) {
		return;
	}

	refresh_apply_button();
	if (applied_check) {
		lv_obj_set_style_image_recolor(applied_check, theme()->accent, 0);
	}
	if (carousel_separator) {
		lv_obj_set_style_bg_color(carousel_separator, theme()->text_secondary, 0);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been inserted, removed or written to since the last
	// visit, so the folder is read again every time.
	scan_entries();
	refresh_carousel();
	refresh_apply_button();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// A preview left open would keep the status bar and the chevron hidden on
	// whatever page comes next. Any animation still running is stopped first:
	// its completion would otherwise touch the page after it has gone.
	lv_anim_delete(&preview_anim_data, preview_anim_cb);
	if (preview_open) {
		lv_obj_set_hidden(preview_layer, true);
		topbar_set_hidden(false);
		back_btn_force_hidden(false);
		preview_open = false;
		preview_animating = false;
		preview_closing = false;
	}

	// A removed card's pictures should not stay in memory. The next visit
	// reads the folder again. The items go before the pictures they draw: an
	// image object must not outlive the buffer it points at.
	lv_obj_clean(carousel);
	carousel_separator = NULL;
	applied_check = NULL;
	clear_entries();
	if (preview_art_loaded) {
		lv_image_set_src(preview_img, NULL);
		cover_free(&preview_art);
		preview_art_loaded = false;
	}
}

void bootscreen_init(gui_config_t *cfg) {
	page_cfg = cfg;
	const bool compact = cfg->screen_width < 320;

	int gap = compact ? 6 : 10;

	lv_obj_add_style(bootscreen_screen, &theme_style_screen, 0);

	int top = settingsrow_content_top(cfg);
	int apply_h = compact ? 44 : 64;
	int content_h = (int)cfg->screen_height - top - apply_h - 2 * cfg->padding;
	if (content_h < 1) {
		content_h = 1;
	}

	// The carousel's large picture is 3:4, the panel's own shape, so what is
	// previewed is what the boot script will draw. It takes the whole height
	// the row has: the picture is the page, not a thumbnail on it.
	item_h = content_h;
	item_w = item_h * 3 / 4;
	if (item_w < 1) {
		item_w = 1;
	}

	carousel_w = (int)cfg->screen_width;
	int carousel_pad = (carousel_w - item_w) / 2;
	if (carousel_pad < 0) {
		carousel_pad = 0;
	}

	lv_obj_t *content = lv_obj_create(bootscreen_screen);
	lv_obj_set_size(content, lv_pct(100), content_h);
	lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(content, 0, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_radius(content, 0, 0);
	lv_obj_set_style_pad_all(content, 0, 0);
	lv_obj_set_style_pad_gap(content, 0, 0);
	lv_obj_set_scrollable(content, false);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The gallery: a row that scrolls sideways, with the middle picture large
	// and the ones either side shrinking away from it. The applied picture is
	// the first item, behind the vertical rule.
	carousel = lv_obj_create(content);
	lv_obj_set_size(carousel, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(carousel, 0, 0);
	lv_obj_set_style_border_width(carousel, 0, 0);
	lv_obj_set_style_radius(carousel, 0, 0);
	lv_obj_set_style_pad_all(carousel, 0, 0);
	lv_obj_set_style_pad_gap(carousel, gap, 0);
	lv_obj_set_style_pad_left(carousel, carousel_pad, 0);
	lv_obj_set_style_pad_right(carousel, carousel_pad, 0);
	lv_obj_set_scrollable(carousel, true);
	lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(carousel, carousel_scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(carousel, carousel_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

	// The one action, at the foot of the page and always in reach.
	apply_btn = lv_btn_create(bootscreen_screen);
	lv_obj_set_size(apply_btn, (int32_t)cfg->screen_width - 2 * cfg->padding, apply_h);
	lv_obj_align(apply_btn, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_style_radius(apply_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(apply_btn, 0, 0);
	lv_obj_set_style_border_width(apply_btn, 0, 0);
	lv_obj_add_event_cb(apply_btn, apply_cb, LV_EVENT_CLICKED, NULL);

	apply_label = lv_label_create(apply_btn);
	lv_obj_set_style_text_font(apply_label, compact ? &font_ui_18 : &font_ui_24, 0);
	lv_obj_center(apply_label);

	// The full-screen preview: a black sheet over the whole page, with the
	// picture on it. It is created last so it sits above the title, the row and
	// the Apply button; the status bar and the chevron live on the top layer
	// and are hidden while it is up.
	preview_layer = lv_obj_create(bootscreen_screen);
	lv_obj_set_size(preview_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(preview_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_color(preview_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(preview_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(preview_layer, 0, 0);
	lv_obj_set_style_radius(preview_layer, 0, 0);
	lv_obj_set_style_pad_all(preview_layer, 0, 0);
	lv_obj_set_scrollable(preview_layer, false);
	lv_obj_set_clickable(preview_layer, true);
	lv_obj_add_event_cb(preview_layer, preview_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(preview_layer, true);

	preview_img = lv_image_create(preview_layer);
	lv_obj_set_size(preview_img, cfg->screen_width, cfg->screen_height);
	lv_image_set_inner_align(preview_img, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_clickable(preview_img, false);
	lv_obj_set_style_transform_pivot_x(preview_img, (int32_t)cfg->screen_width / 2, 0);
	lv_obj_set_style_transform_pivot_y(preview_img, (int32_t)cfg->screen_height / 2, 0);

	lv_obj_add_event_cb(bootscreen_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(bootscreen_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(bootscreen_screen, preview_back_guard);
	switcher_set_back_guard_peek(bootscreen_screen, preview_back_guard_peek);
	theme_register_refresh(repaint_cb);

	// The gallery is read when the page is opened, not here: the card may not
	// have been mounted yet, and a page the user never visits should not spend
	// startup decoding pictures.
	refresh_apply_button();
}
