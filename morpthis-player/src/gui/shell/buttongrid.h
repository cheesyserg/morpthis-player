#ifndef BUTTONGRID_H
#define BUTTONGRID_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// A grid of buttons whose per-frame cost does not grow with the number of
// items it holds.
//
// LVGL resolves a child's styles before it tests whether the child is on
// screen, so a flat container of 200 buttons costs 200 style cascades every
// frame even when 180 of them are scrolled out of view. This builds the grid
// as one container per few rows, positions everything with fixed coordinates,
// and paints it flat:
//
//   * off-screen groups fail the intersection test at the group, so their
//     buttons are never visited;
//   * radius is 0 and shadows are off on every cell, so no corner or shadow
//     masks are generated per cell (the mask caches are 32x32 and 8 radii);
//   * the scrolling surface is opaque, so LVGL starts drawing at it instead
//     of walking what is behind it;
//   * nothing is measured or laid out again while scrolling: no flex, no grid,
//     every position is set once.
//
// Text-only grids can use a single lv_buttonmatrix instead (`text_only`),
// which is one object for the whole grid: no per-cell style resolution at all,
// at the cost of having no icon per cell.

// One cell. `label` and `icon` are not copied: the array and the strings must
// outlive the grid.
typedef struct {
	const char *label;
	const lv_image_dsc_t *icon; // optional; ignored by the text-only path
	void *user;                 // handed back to the click callback
} buttongrid_item_t;

// `index` is the item's index, `item_user` its `user`, `grid_user` what was
// passed to buttongrid_create().
typedef void (*buttongrid_click_cb_t)(int index, void *item_user, void *grid_user);

typedef struct {
	int32_t x, y;       // where the scrolling surface sits in the parent
	int32_t width;      // the viewport's size; the grid scrolls inside it
	int32_t height;
	int32_t columns;
	int32_t cell_w;     // one cell, before `gap`
	int32_t cell_h;
	int32_t gap;        // between cells, both ways

	// Rows per culling group. 0 means one row per group, which is the
	// cheapest: a group that is off screen costs one style lookup, not one per
	// cell. Larger groups mean fewer objects but more cells visited per
	// partially visible group.
	int32_t rows_per_chunk;

	bool text_only;     // one lv_buttonmatrix instead of a widget per cell
	bool scrollbar;     // show the scroll bar while scrolling

	// The label font. NULL uses font_ui_18.
	const lv_font_t *font;
} buttongrid_cfg_t;

// Builds the grid under `parent` and returns the scrolling surface (the object
// to attach to, position, or delete). NULL when there is nothing to build.
lv_obj_t *buttongrid_create(lv_obj_t *parent, const buttongrid_cfg_t *cfg, const buttongrid_item_t *items, int count,
							buttongrid_click_cb_t cb, void *user);

#endif // BUTTONGRID_H
