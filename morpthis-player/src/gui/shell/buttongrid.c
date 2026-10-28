#include "buttongrid.h"

#include <stdint.h>
#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"

// The three properties that matter for cost, on top of the theme's card
// colours: flat corners (no corner masks), no shadow (no shadow masks), and a
// fully opaque fill (so the page behind a cell is never composited through
// it). Added after the theme styles, so they win, and shared by every cell so
// it costs three properties in total rather than three per button.
static lv_style_t style_flat;
static bool styles_ready;

static void styles_init(void) {
	if (styles_ready) {
		return;
	}
	lv_style_init(&style_flat);
	lv_style_set_radius(&style_flat, 0);
	lv_style_set_shadow_width(&style_flat, 0);
	lv_style_set_bg_opa(&style_flat, LV_OPA_COVER);
	styles_ready = true;
}

// One per grid. The text-only path owns the map array (lv_buttonmatrix keeps a
// reference to it); both paths own the context until the surface is deleted.
typedef struct {
	const buttongrid_item_t *items;
	int count;
	buttongrid_click_cb_t cb;
	void *user;
	char **map;
} buttongrid_ctx_t;

static void grid_free_cb(lv_event_t *e) {
	buttongrid_ctx_t *ctx = lv_event_get_user_data(e);
	if (!ctx) {
		return;
	}
	free(ctx->map);
	ctx->map = NULL;
	free(ctx);
}

// The index rides on the button's own user_data, so a cell costs no allocation
// of its own. +1 keeps the first cell distinguishable from "no user data".
static void cell_clicked_cb(lv_event_t *e) {
	buttongrid_ctx_t *ctx = lv_event_get_user_data(e);
	if (!ctx || !ctx->cb) {
		return;
	}

	int index = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)) - 1;
	if (index < 0 || index >= ctx->count) {
		return;
	}
	ctx->cb(index, ctx->items[index].user, ctx->user);
}

static void matrix_value_cb(lv_event_t *e) {
	buttongrid_ctx_t *ctx = lv_event_get_user_data(e);
	if (!ctx || !ctx->cb) {
		return;
	}

	lv_obj_t *matrix = lv_event_get_current_target(e);
	uint32_t id = lv_buttonmatrix_get_selected_button(matrix);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || id >= (uint32_t)ctx->count) {
		return;
	}
	ctx->cb((int)id, ctx->items[id].user, ctx->user);
}

// The scrolling viewport: the one opaque surface the grid sits on. Radius 0 and
// no border or shadow keep it a plain rectangle, which is what lets LVGL's
// occlusion test start drawing here instead of at the page underneath.
static void style_surface(lv_obj_t *surface, const buttongrid_cfg_t *cfg) {
	lv_obj_add_style(surface, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(surface, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(surface, 0, 0);
	lv_obj_set_style_shadow_width(surface, 0, 0);
	lv_obj_set_style_border_width(surface, 0, 0);
	lv_obj_set_style_pad_all(surface, 0, 0);
	lv_obj_set_pos(surface, cfg->x, cfg->y);
	lv_obj_set_size(surface, cfg->width, cfg->height);

	lv_obj_set_scrollable(surface, true);
	lv_obj_set_scroll_dir(surface, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(surface, cfg->scrollbar ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
	// A press that starts on a cell is the start of a drag, not a request to
	// focus that cell and scroll it into view.
	lv_obj_set_scroll_on_focus(surface, false);
}

static void style_cell(lv_obj_t *cell) {
	lv_obj_remove_style_all(cell);
	lv_obj_add_style(cell, &theme_style_card, 0);
	lv_obj_add_style(cell, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_add_style(cell, &style_flat, 0);
}

static void make_cell(lv_obj_t *chunk, int column, int row_in_chunk, const buttongrid_cfg_t *cfg,
					  const buttongrid_item_t *item, int index, buttongrid_ctx_t *ctx, const lv_font_t *font) {
	lv_obj_t *cell = lv_button_create(chunk);
	style_cell(cell);
	lv_obj_set_size(cell, cfg->cell_w, cfg->cell_h);
	lv_obj_set_pos(cell, column * (cfg->cell_w + cfg->gap), row_in_chunk * (cfg->cell_h + cfg->gap));
	lv_obj_set_user_data(cell, (void *)(uintptr_t)(index + 1));
	lv_obj_add_event_cb(cell, cell_clicked_cb, LV_EVENT_CLICKED, ctx);

	if (item->icon) {
		lv_obj_t *icon = lv_image_create(cell);
		lv_obj_remove_style_all(icon);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_image_set_src(icon, item->icon);
		lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);
	}

	lv_obj_t *label = lv_label_create(cell);
	lv_obj_remove_style_all(label);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_label_set_text_static(label, item->label ? item->label : "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	// Alignment is resolved once, here. Nothing about it changes while the
	// grid scrolls, so no layout pass is ever triggered by scrolling.
	if (item->icon) {
		lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
	} else {
		lv_obj_center(label);
	}
}

// One child container per `rows_per_chunk` rows. Its whole point is culling:
// lv_obj_refr() resolves a child's styles before it tests visibility, so an
// off-screen *group* costs one cascade and stops there, where a flat container
// would cost one per button inside it.
static lv_obj_t *build_chunked(lv_obj_t *parent, const buttongrid_cfg_t *cfg, const buttongrid_item_t *items, int count,
							   buttongrid_ctx_t *ctx, const lv_font_t *font) {
	int rows = (count + cfg->columns - 1) / cfg->columns;
	int rows_per_chunk = cfg->rows_per_chunk > 0 ? cfg->rows_per_chunk : 1;

	lv_obj_t *surface = lv_obj_create(parent);
	style_surface(surface, cfg);

	for (int first_row = 0; first_row < rows; first_row += rows_per_chunk) {
		int chunk_rows = LV_MIN(rows_per_chunk, rows - first_row);

		lv_obj_t *chunk = lv_obj_create(surface);
		lv_obj_remove_style_all(chunk);
		lv_obj_set_scrollable(chunk, false);
		lv_obj_set_clickable(chunk, false);
		lv_obj_set_size(chunk, cfg->width,
						(chunk_rows - 1) * (cfg->cell_h + cfg->gap) + cfg->cell_h);
		lv_obj_set_pos(chunk, 0, first_row * (cfg->cell_h + cfg->gap));

		for (int r = 0; r < chunk_rows; r++) {
			for (int c = 0; c < cfg->columns; c++) {
				int index = (first_row + r) * cfg->columns + c;
				if (index >= count) {
					break;
				}
				make_cell(chunk, c, r, cfg, &items[index], index, ctx, font);
			}
		}
	}

	lv_obj_add_event_cb(surface, grid_free_cb, LV_EVENT_DELETE, ctx);
	return surface;
}

// Text-only grids are one object: no per-cell style resolution at all, and the
// whole grid scrolls as a single child. Icons are the price; when a cell needs
// one, use the chunked path.
static lv_obj_t *build_matrix(lv_obj_t *parent, const buttongrid_cfg_t *cfg, const buttongrid_item_t *items, int count,
							  buttongrid_ctx_t *ctx, const lv_font_t *font) {
	int rows = (count + cfg->columns - 1) / cfg->columns;

	// count labels, one "\n" per row boundary, and the terminating NULL.
	ctx->map = calloc((size_t)count + (size_t)rows, sizeof(*ctx->map));
	if (!ctx->map) {
		return NULL;
	}
	int at = 0;
	for (int i = 0; i < count; i++) {
		ctx->map[at++] = (char *)(items[i].label ? items[i].label : "");
		if ((i % cfg->columns) == cfg->columns - 1 && i + 1 < count) {
			ctx->map[at++] = "\n";
		}
	}
	ctx->map[at] = NULL;

	lv_obj_t *surface = lv_obj_create(parent);
	style_surface(surface, cfg);

	lv_obj_t *matrix = lv_buttonmatrix_create(surface);
	lv_obj_remove_style_all(matrix);
	lv_obj_set_scrollable(matrix, false);
	lv_obj_set_pos(matrix, 0, 0);
	lv_obj_set_size(matrix, cfg->width, (rows - 1) * (cfg->cell_h + cfg->gap) + cfg->cell_h);

	// The grid itself paints nothing; the surface below it is the opaque fill.
	lv_obj_set_style_bg_opa(matrix, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_border_width(matrix, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(matrix, 0, LV_PART_MAIN);
	lv_obj_set_style_shadow_width(matrix, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_row(matrix, cfg->gap, LV_PART_MAIN);
	lv_obj_set_style_pad_column(matrix, cfg->gap, LV_PART_MAIN);

	lv_obj_add_style(matrix, &theme_style_card, LV_PART_ITEMS);
	lv_obj_add_style(matrix, &theme_style_card_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_add_style(matrix, &style_flat, LV_PART_ITEMS);
	lv_obj_add_style(matrix, &theme_style_text, LV_PART_ITEMS);
	lv_obj_set_style_text_font(matrix, font, LV_PART_ITEMS);

	lv_buttonmatrix_set_map(matrix, (const char *const *)ctx->map);

	lv_buttonmatrix_ctrl_t *ctrl = calloc((size_t)count, sizeof(*ctrl));
	if (ctrl) {
		for (int i = 0; i < count; i++) {
			ctrl[i] = (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_WIDTH_1 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
											   LV_BUTTONMATRIX_CTRL_NO_REPEAT);
		}
		lv_buttonmatrix_set_ctrl_map(matrix, ctrl);
		free(ctrl);
	}

	// VALUE_CHANGED rather than PRESSED: CLICK_TRIG makes it fire on release,
	// which is the behaviour every button on these pages has.
	lv_obj_add_event_cb(matrix, matrix_value_cb, LV_EVENT_VALUE_CHANGED, ctx);
	lv_obj_add_event_cb(matrix, grid_free_cb, LV_EVENT_DELETE, ctx);
	return surface;
}

lv_obj_t *buttongrid_create(lv_obj_t *parent, const buttongrid_cfg_t *cfg, const buttongrid_item_t *items, int count,
							buttongrid_click_cb_t cb, void *user) {
	if (!parent || !cfg || !items || count <= 0 || cfg->columns <= 0) {
		return NULL;
	}

	styles_init();
	const lv_font_t *font = cfg->font ? cfg->font : &font_ui_18;

	buttongrid_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return NULL;
	}
	ctx->items = items;
	ctx->count = count;
	ctx->cb = cb;
	ctx->user = user;

	lv_obj_t *surface = cfg->text_only ? build_matrix(parent, cfg, items, count, ctx, font)
									   : build_chunked(parent, cfg, items, count, ctx, font);
	if (!surface) {
		free(ctx->map);
		free(ctx);
		return NULL;
	}
	return surface;
}
