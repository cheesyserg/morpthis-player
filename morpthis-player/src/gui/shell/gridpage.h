#ifndef GRIDPAGE_H
#define GRIDPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The tiled page layout, shared by the main menu and the Music section so the
// two look and behave identically.

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;

	// Where the tile goes. NULL for a section that does not exist yet: the
	// tile is dimmed and says so when tapped, rather than silently doing
	// nothing.
	lv_obj_t **target;

	// Alternative to `target` for tiles whose destination needs preparing
	// before it can be shown (the library index pages load their list first).
	// Checked only when `target` is NULL; entries that set neither are the
	// dimmed not-yet-available tiles.
	void (*action)(void);
} grid_entry_t;

// Fills `screen` with a `columns` x `rows` grid of tiles sized from the config.
// The grid is also a surface the player can be dragged in from, including from
// the tiles themselves.
// `clear_corner_buttons` starts the grid below the floating back button and
// any icon in the opposite corner, so the cards never sit under them.
// Repaints one tile in place: its picture and its caption. For the page whose
// tiles are not fixed -- Music, where Browse and Playlists trade places -- so
// the grid does not have to be built again to swap two of them.
void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label);

// Replaces a tile's image and optionally tints it with a Tokyo Night semantic
// colour. `icon_size` is the displayed long side (the source is scaled without
// changing its pixel data); `glow` adds a small, static low-opacity echo behind
// the glyph. Passing glow=false hides an echo that was created earlier.
void gridpage_set_tile_icon_style(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, int icon_size,
								  bool recolor, lv_color_t color, bool glow);

// Sets a tile's contents in a horizontal icon-and-label row, or restores the
// original centered vertical layout. The horizontal form is used by the V1's
// Tokyo Night home menu; existing themes keep their original construction.
void gridpage_set_tile_orientation(lv_obj_t *grid, int index, bool horizontal);

// Hides a tile caption without deleting its text or changing other grids.
// Pair with the vertical orientation to center an icon on its own.
void gridpage_set_tile_label_hidden(lv_obj_t *grid, int index, bool hidden);

// Lets selected Tokyo Night captions use up to `lines` tidy lines instead of
// the default one-line ellipsis (for labels such as File Manager and Album
// artist). The caller restores one line when returning to Dark or Light.
void gridpage_set_tile_label_lines(lv_obj_t *grid, int index, int lines);

// Reflows an existing grid into a different screen area. Used by the V1's
// Tokyo Night home screen to reserve space for its heading and mini-player,
// without duplicating the six tile objects or changing Dark/Light geometry.
void gridpage_set_layout(lv_obj_t *grid, gui_config_t *cfg, int top, int bottom, int padding, int gap, int columns,
						 int rows);

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns, int rows,
					bool clear_corner_buttons);
void gridpage_replace_entries(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
							  int rows);

// What a section page shows instead of its tiles while its index is empty: a
// picture, a line saying a scan is needed and a Scan button calling `scan_cb`.
// Hidden until gridpage_show_empty() is told otherwise.
lv_obj_t *gridpage_empty_panel(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *icon, const char *text,
							   lv_event_cb_t scan_cb);

// Either the tiles or the panel.
void gridpage_show_empty(lv_obj_t *grid, lv_obj_t *panel, bool empty);

#endif /* GRIDPAGE_H */
