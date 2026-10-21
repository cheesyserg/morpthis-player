#include "bootscreen.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
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

typedef enum {
	ENTRY_STOCK,
	ENTRY_RETROSPACE,
	ENTRY_CUSTOM,
} entry_kind_t;

typedef struct {
	entry_kind_t kind;
	char name[BOOTSCREEN_NAME_MAX]; // the file name, or "Stock"/"Retrospace"
	char path[BOOTSCREEN_PATH_MAX];
	cover_image_t thumb;
	bool thumb_loaded;
} entry_t;

static gui_config_t *page_cfg;
static entry_t entries[BOOTSCREEN_MAX];
static int entry_count;
static int selected = -1;
static int applied_entry = -1;

static lv_obj_t *preview_img;
static lv_obj_t *preview_placeholder;
static lv_obj_t *preview_name;
static lv_obj_t *applied_img;
static lv_obj_t *applied_label;
static lv_obj_t *applied_check;
static lv_obj_t *thumbs_row;
static lv_obj_t *thumb_btn[BOOTSCREEN_MAX];
static lv_obj_t *empty_label;
static lv_obj_t *apply_btn;
static lv_obj_t *apply_label;

static cover_image_t preview;
static bool preview_loaded;
static cover_image_t applied_thumb;
static bool applied_loaded;

static int preview_box_w;
static int preview_box_h;
static int applied_w;
static int applied_h;
static int thumb_size;

// Draws one of the interface's glyphs at `size`, whatever its source art is.
// The check on the applied picture and on its thumbnail are the two users.
static void scale_icon(lv_obj_t *icon, const lv_image_dsc_t *source, int size) {
	lv_obj_set_size(icon, size, size);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	int source_side = LV_MAX((int)source->header.w, (int)source->header.h);
	if (source_side > 0) {
		lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * size / source_side));
	}
}

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

static void find_applied_entry(void) {
	applied_entry = -1;
	for (int i = 0; i < entry_count; i++) {
		if (entry_is_applied(&entries[i])) {
			applied_entry = i;
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// The gallery
// ---------------------------------------------------------------------------

static void free_entry(entry_t *entry) {
	if (entry->thumb_loaded) {
		cover_free(&entry->thumb);
		entry->thumb_loaded = false;
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

	// The two pictures the firmware itself carries. Stock follows the theme,
	// so the preview is the one the current palette would draw.
	add_entry(ENTRY_STOCK, "Stock", theme()->dark ? "/etc/logo2.jpeg" : "/etc/logo1.jpeg");
	add_entry(ENTRY_RETROSPACE, "Retrospace", "/etc/logo.jpeg");

	const char *root = storage_sd_root();
	if (root && root[0]) {
		char dir[BOOTSCREEN_PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/" BOOTSCREEN_DIR, root);

		struct dirent **list = NULL;
		int n = scandir(dir, &list, NULL, alphasort);
		if (n > 0) {
			for (int i = 0; i < n; i++) {
				const char *name = list[i]->d_name;
				if (name[0] != '.' && image_file(name)) {
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

	find_applied_entry();
	selected = applied_entry >= 0 ? applied_entry : 0;
}

static void load_thumb(entry_t *entry) {
	if (entry->thumb_loaded) {
		return;
	}
	if (cover_load_image_file(entry->path, thumb_size, thumb_size, COVER_FIT_COVER, &entry->thumb)) {
		entry->thumb_loaded = true;
	}
}

static void load_preview(void) {
	if (preview_loaded) {
		lv_image_set_src(preview_img, NULL);
		cover_free(&preview);
		preview_loaded = false;
	}

	if (selected < 0 || selected >= entry_count) {
		lv_obj_set_hidden(preview_placeholder, false);
		lv_label_set_text(preview_name, "");
		return;
	}

	entry_t *entry = &entries[selected];
	if (cover_load_image_file(entry->path, preview_box_w, preview_box_h, COVER_FIT_CONTAIN, &preview)) {
		preview_loaded = true;
		lv_obj_set_size(preview_img, (int32_t)preview.dsc.header.w, (int32_t)preview.dsc.header.h);
		lv_image_set_src(preview_img, &preview.dsc);
		lv_obj_center(preview_img);
		lv_obj_set_hidden(preview_placeholder, true);
	} else {
		lv_obj_set_size(preview_img, 0, 0);
		lv_image_set_src(preview_img, NULL);
		lv_obj_set_hidden(preview_placeholder, false);
	}

	lv_label_set_text(preview_name, entry->name);
}

static void refresh_applied(void) {
	if (applied_loaded) {
		lv_image_set_src(applied_img, NULL);
		cover_free(&applied_thumb);
		applied_loaded = false;
	}

	const char *path = applied_path();
	if (path && cover_load_image_file(path, applied_w, applied_h, COVER_FIT_COVER, &applied_thumb)) {
		applied_loaded = true;
		lv_obj_set_size(applied_img, applied_w, applied_h);
		lv_image_set_src(applied_img, &applied_thumb.dsc);
		lv_obj_center(applied_img);
	} else {
		lv_obj_set_size(applied_img, 0, 0);
		lv_image_set_src(applied_img, NULL);
	}

	lv_label_set_text_fmt(applied_label, "%s: %s", tr("bootscreen_applied"), bootscreen_applied_name());
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

static void thumb_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count || index == selected) {
		return;
	}

	selected = index;
	load_preview();
	refresh_apply_button();

	// The selected border is the only thing that moves on the row; the
	// pictures themselves are already decoded.
	for (int i = 0; i < entry_count; i++) {
		if (thumb_btn[i]) {
			lv_obj_set_style_border_width(thumb_btn[i], i == selected ? 2 : 0, 0);
			lv_obj_set_style_border_color(thumb_btn[i], theme()->accent, 0);
		}
	}
}

static void refresh_thumbs(void) {
	lv_obj_clean(thumbs_row);
	for (int i = 0; i < BOOTSCREEN_MAX; i++) {
		thumb_btn[i] = NULL;
	}

	for (int i = 0; i < entry_count; i++) {
		entry_t *entry = &entries[i];
		load_thumb(entry);

		lv_obj_t *btn = lv_btn_create(thumbs_row);
		thumb_btn[i] = btn;
		lv_obj_set_size(btn, thumb_size, thumb_size);
		lv_obj_set_style_radius(btn, bp_pick(8, 12), 0);
		lv_obj_set_style_bg_color(btn, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(btn, i == selected ? 2 : 0, 0);
		lv_obj_set_style_border_color(btn, theme()->accent, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);
		lv_obj_add_event_cb(btn, thumb_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		if (entry->thumb_loaded) {
			lv_obj_t *img = lv_image_create(btn);
			lv_image_set_src(img, &entry->thumb.dsc);
			lv_obj_set_size(img, thumb_size, thumb_size);
			lv_obj_center(img);
		} else {
			lv_obj_t *icon = lv_image_create(btn);
			lv_image_set_src(icon, &icon_files_image);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			lv_obj_center(icon);
		}

		if (i == applied_entry) {
			lv_obj_t *check = lv_image_create(btn);
			lv_image_set_src(check, &icon_check);
			lv_obj_add_style(check, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor(check, theme()->accent, 0);
			lv_obj_set_style_image_recolor_opa(check, LV_OPA_COVER, 0);
			scale_icon(check, &icon_check, bp_pick(16, 22));
			lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -2, 2);
		}
	}

	// The built-ins are always there, so the note is only for a card with no
	// pictures of its own in the folder.
	lv_obj_set_hidden(empty_label, entry_count > 2);
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

static void apply_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (selected < 0 || selected >= entry_count) {
		return;
	}

	entry_t *entry = &entries[selected];
	if (entry_is_applied(entry)) {
		return;
	}

	if (entry->kind == ENTRY_STOCK) {
		config_set("ui", "boot_screen_name", "");
		theme_set_boot_screen(BOOTLOGO_STOCK);
	} else if (entry->kind == ENTRY_RETROSPACE) {
		config_set("ui", "boot_screen_name", "");
		theme_set_boot_screen(BOOTLOGO_RETROSPACE);
	} else {
		uint8_t *jpeg = NULL;
		size_t len = 0;
		if (!encode_custom(entry->path, &jpeg, &len)) {
			toast_error("bootscreen_apply_failed");
			return;
		}

		bool stored = bootlogo_custom_store(jpeg, len);
		free(jpeg);
		if (!stored) {
			toast_error("bootscreen_apply_failed");
			return;
		}

		config_set("ui", "boot_screen_name", entry->name);
		theme_set_boot_screen(BOOTLOGO_CUSTOM);
	}

	find_applied_entry();
	refresh_applied();
	refresh_thumbs();
	refresh_apply_button();
	toast_success("bootscreen_applied_toast");
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

static void repaint_cb(void) {
	if (!apply_btn) {
		return;
	}

	refresh_apply_button();
	for (int i = 0; i < entry_count; i++) {
		if (thumb_btn[i]) {
			lv_obj_set_style_border_width(thumb_btn[i], i == selected ? 2 : 0, 0);
			lv_obj_set_style_border_color(thumb_btn[i], theme()->accent, 0);
			lv_obj_set_style_bg_color(thumb_btn[i], theme()->surface_pressed, 0);
		}
	}
	if (applied_check) {
		lv_obj_set_style_image_recolor(applied_check, theme()->accent, 0);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The page owns the whole panel: the status bar steps aside while it is up.
	topbar_set_hidden(true);

	// The card may have been inserted, removed or written to since the last
	// visit, so the folder is read again every time.
	scan_entries();
	refresh_applied();
	refresh_thumbs();
	load_preview();
	refresh_apply_button();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// A removed card's pictures should not stay in memory. The next visit
	// reads the folder again. The buttons go before the pictures they draw:
	// an image object must not outlive the buffer it points at.
	lv_obj_clean(thumbs_row);
	clear_entries();
	if (preview_loaded) {
		lv_image_set_src(preview_img, NULL);
		cover_free(&preview);
		preview_loaded = false;
	}
	if (applied_loaded) {
		lv_image_set_src(applied_img, NULL);
		cover_free(&applied_thumb);
		applied_loaded = false;
	}
}

void bootscreen_init(gui_config_t *cfg) {
	page_cfg = cfg;
	const bool compact = cfg->screen_width < 320;

	thumb_size = compact ? 44 : 64;
	applied_w = compact ? 24 : 36;
	applied_h = compact ? 32 : 48;

	int preview_h = compact ? 140 : 260;
	int preview_pad = compact ? 4 : 10;
	int name_h = lv_font_get_line_height(compact ? &font_ui_16 : &font_ui_22);
	preview_box_w = (int)cfg->screen_width - 2 * cfg->padding - 2 * preview_pad;
	preview_box_h = preview_h - 2 * preview_pad - name_h - (compact ? 2 : 6);
	if (preview_box_w < 1) {
		preview_box_w = 1;
	}
	if (preview_box_h < 1) {
		preview_box_h = 1;
	}

	lv_obj_add_style(bootscreen_screen, &theme_style_screen, 0);
	settingsrow_title(bootscreen_screen, cfg, "appearance_boot_screen");

	int top = settingsrow_content_top(cfg);
	int apply_h = compact ? 44 : 64;
	int content_h = (int)cfg->screen_height - top - apply_h - 2 * cfg->padding;
	if (content_h < 1) {
		content_h = 1;
	}

	lv_obj_t *content = lv_obj_create(bootscreen_screen);
	lv_obj_set_size(content, lv_pct(100), content_h);
	lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(content, 0, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_radius(content, 0, 0);
	lv_obj_set_style_pad_all(content, cfg->padding, 0);
	lv_obj_set_style_pad_gap(content, compact ? 6 : 12, 0);
	lv_obj_set_scrollable(content, true);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// The applied picture, in a section of its own: the one thing on the page
	// that says what the device will actually draw.
	lv_obj_t *applied_card = lv_obj_create(content);
	lv_obj_set_width(applied_card, lv_pct(100));
	lv_obj_set_height(applied_card, compact ? 40 : 64);
	lv_obj_add_style(applied_card, &theme_style_card, 0);
	lv_obj_set_style_radius(applied_card, compact ? 8 : 12, 0);
	lv_obj_set_style_border_width(applied_card, 0, 0);
	lv_obj_set_style_shadow_width(applied_card, 0, 0);
	lv_obj_set_style_pad_all(applied_card, compact ? 4 : 8, 0);
	lv_obj_set_style_pad_gap(applied_card, compact ? 6 : 10, 0);
	lv_obj_set_scrollable(applied_card, false);
	lv_obj_set_flex_flow(applied_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(applied_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	applied_img = lv_image_create(applied_card);
	lv_obj_set_size(applied_img, applied_w, applied_h);
	lv_obj_set_clickable(applied_img, false);

	applied_label = lv_label_create(applied_card);
	lv_obj_set_flex_grow(applied_label, 1);
	lv_label_set_long_mode(applied_label, LV_LABEL_LONG_DOT);
	lv_obj_add_style(applied_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(applied_label, compact ? &font_ui_16 : &font_ui_22, 0);

	applied_check = lv_image_create(applied_card);
	lv_image_set_src(applied_check, &icon_check);
	lv_obj_add_style(applied_check, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(applied_check, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(applied_check, LV_OPA_COVER, 0);
	scale_icon(applied_check, &icon_check, compact ? 16 : 24);

	// The selected picture, large enough to judge.
	lv_obj_t *preview_card = lv_obj_create(content);
	lv_obj_set_width(preview_card, lv_pct(100));
	lv_obj_set_height(preview_card, preview_h);
	lv_obj_add_style(preview_card, &theme_style_card, 0);
	lv_obj_set_style_radius(preview_card, compact ? 10 : 14, 0);
	lv_obj_set_style_border_width(preview_card, 0, 0);
	lv_obj_set_style_shadow_width(preview_card, 0, 0);
	lv_obj_set_style_pad_all(preview_card, preview_pad, 0);
	lv_obj_set_scrollable(preview_card, false);
	lv_obj_set_flex_flow(preview_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(preview_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	preview_img = lv_image_create(preview_card);
	lv_image_set_inner_align(preview_img, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_clickable(preview_img, false);

	preview_placeholder = lv_image_create(preview_card);
	lv_image_set_src(preview_placeholder, &icon_files_image);
	lv_obj_add_style(preview_placeholder, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(preview_placeholder, LV_OPA_40, 0);

	preview_name = lv_label_create(preview_card);
	lv_label_set_long_mode(preview_name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(preview_name, lv_pct(100));
	lv_obj_set_style_text_align(preview_name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(preview_name, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(preview_name, compact ? &font_ui_16 : &font_ui_22, 0);

	// The gallery itself: a row that scrolls sideways, so a card full of
	// pictures does not push the preview off the page.
	thumbs_row = lv_obj_create(content);
	lv_obj_set_width(thumbs_row, lv_pct(100));
	lv_obj_set_height(thumbs_row, thumb_size + (compact ? 8 : 16));
	lv_obj_set_style_bg_opa(thumbs_row, 0, 0);
	lv_obj_set_style_border_width(thumbs_row, 0, 0);
	lv_obj_set_style_pad_all(thumbs_row, 0, 0);
	lv_obj_set_style_pad_gap(thumbs_row, compact ? 6 : 10, 0);
	lv_obj_set_scrollable(thumbs_row, true);
	lv_obj_set_scroll_dir(thumbs_row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(thumbs_row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(thumbs_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(thumbs_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	empty_label = lv_label_create(content);
	lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_label, lv_pct(100));
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, compact ? &font_ui_16 : &font_ui_22, 0);
	lv_label_set_text(empty_label, tr("bootscreen_empty"));

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

	lv_obj_add_event_cb(bootscreen_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(bootscreen_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	theme_register_refresh(repaint_cb);

	// The gallery is read when the page is opened, not here: the card may not
	// have been mounted yet, and a page the user never visits should not spend
	// the startup decoding thumbnails.
	refresh_apply_button();
}
