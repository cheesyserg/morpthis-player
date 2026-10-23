#include "gridpage.h"

#include "lvgl/lvgl.h"

#include <string.h>

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define GRID_GAP 14
#define COMPACT_TILE_ICON_MAX 46
// The empty-state art is alone on the page rather than inside a 75 px tile, so
// it can be bigger than a tile icon and still leave the sentence and the scan
// button their room.
#define COMPACT_EMPTY_ICON_MAX 56

static bool compact_grid(void) { return bp_is_tempotec_v1(); }
static int grid_gap(void) { return compact_grid() ? 6 : GRID_GAP; }

// A compact list of the tiled menu pages. The entry arrays passed to
// gridpage_build() are local to each init function, so retain just the icon
// and translation key that are needed to restore Dark/Light or apply Tokyo
// Night later. V1 has six tiles per page; the extra slots cover optional
// builds without allocating per-tile theme objects.
#define GRIDPAGE_THEME_PAGE_MAX 12
#define GRIDPAGE_THEME_TILE_MAX 16
typedef struct {
	lv_obj_t *grid;
	int count;
	const lv_image_dsc_t *normal_icons[GRIDPAGE_THEME_TILE_MAX];
	const char *labels[GRIDPAGE_THEME_TILE_MAX];
	// A tile whose icon is a colour drawing (an app's own icon) keeps it in
	// every layout: no recolour, no Tokyo Night replacement.
	bool keep_color[GRIDPAGE_THEME_TILE_MAX];
	bool wide_first;
} gridpage_theme_page_t;

static gridpage_theme_page_t theme_pages[GRIDPAGE_THEME_PAGE_MAX];
static int theme_page_count;
static bool theme_refresh_registered;

static void gridpage_apply_theme_page(gridpage_theme_page_t *page);
static gridpage_theme_page_t *gridpage_theme_page_for(lv_obj_t *grid);

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // this was a swipe across the tile, not a tap on it
	}

	const char *label = lv_event_get_user_data(e);
	char message[160];

	// tr() on the way in as well: what is stored on the tile is the tag, and
	// the message has to name the tile the way the tile does.
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
}

static void action_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // a swipe, not a tap
	}

	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	action();
}

// A tile on a page rebuilt from data (the apps page): the entry carries the
// action and its argument. The entry must outlive the tile, which is why the
// apps page keeps its entries in a static array.
static void action_user_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return; // a swipe, not a tap
	}

	const grid_entry_t *entry = lv_event_get_user_data(e);
	if (entry && entry->action_user) {
		entry->action_user(entry->user);
	}
}

// The two children add_tile() puts on every tile, in the order it makes them.
#define TILE_ICON_CHILD 0
#define TILE_LABEL_CHILD 1

static bool tile_has_icon_halo(lv_obj_t *tile) { return tile && lv_obj_get_child_count(tile) >= 3; }

static lv_obj_t *tile_icon_object(lv_obj_t *tile) {
	return lv_obj_get_child(tile, tile_has_icon_halo(tile) ? 1 : TILE_ICON_CHILD);
}

static lv_obj_t *tile_label_object(lv_obj_t *tile) {
	return lv_obj_get_child(tile, tile_has_icon_halo(tile) ? 2 : TILE_LABEL_CHILD);
}

void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}
	lv_obj_t *tile = lv_obj_get_child(grid, index);
	if (!tile) {
		return;
	}
	if (icon) {
		lv_image_set_src(tile_icon_object(tile), icon);
	}
	if (label) {
		lv_label_set_text(tile_label_object(tile), tr(label));
	}

	// Music swaps this slot between Browse and Playlists at runtime. Keep the
	// original target and label in sync so a theme switch never restores stale
	// menu art, and immediately reapply the selected icon treatment.
	gridpage_theme_page_t *page = gridpage_theme_page_for(grid);
	if (page && index < page->count) {
		if (icon) {
			page->normal_icons[index] = icon;
		}
		if (label) {
			page->labels[index] = label;
		}
		gridpage_apply_theme_page(page);
	}
}

// Sets a tile glyph at a known display size. The optional echo is a separate
// static image object: it costs no bitmap memory, stays outside flex layout,
// and is created only for the handful of Tokyo Night home-menu icons.
void gridpage_set_tile_icon_style(lv_obj_t *grid, int index, const lv_image_dsc_t *source, int icon_size,
								  bool recolor, lv_color_t color, bool glow) {
	if (!grid || !source || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}

	lv_obj_t *tile = lv_obj_get_child(grid, index);
	if (!tile) {
		return;
	}

	lv_obj_t *halo = NULL;
	if (tile_has_icon_halo(tile)) {
		halo = lv_obj_get_child(tile, 0);
	} else if (glow) {
		halo = lv_image_create(tile);
		lv_obj_set_floating(halo, true);
		lv_obj_set_clickable(halo, false);
		lv_obj_set_scrollable(halo, false);
		lv_obj_move_to_index(halo, 0); // behind the sharp icon and label
	}

	lv_obj_t *icon = tile_icon_object(tile);
	if (!icon) {
		return;
	}
	lv_image_set_src(icon, source);

	int source_w = (int)source->header.w;
	int source_h = (int)source->header.h;
	int max_side = LV_MAX(source_w, source_h);
	if (max_side <= 0) {
		return;
	}
	int target_side = icon_size > 0 ? icon_size : max_side;
	int icon_w = LV_MAX(1, source_w * target_side / max_side);
	int icon_h = LV_MAX(1, source_h * target_side / max_side);
	uint32_t icon_scale = (uint32_t)(LV_SCALE_NONE * target_side / max_side);
	if (icon_scale == 0) {
		icon_scale = 1;
	}

	lv_obj_set_size(icon, icon_w, icon_h);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(icon, icon_scale);
	if (recolor) {
		lv_obj_set_style_image_recolor(icon, color, 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}

	if (!halo) {
		return;
	}
	lv_image_set_src(halo, source);
	lv_obj_set_style_bg_opa(halo, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(halo, 0, 0);
	lv_obj_set_style_image_opa(halo, LV_OPA_20, 0);
	if (recolor) {
		lv_obj_set_style_image_recolor(halo, color, 0);
		lv_obj_set_style_image_recolor_opa(halo, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(halo, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(halo, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}

	// The larger copy is still a single source glyph and is static. It is not
	// a blur, animation or screen-sized buffer, so the draw cost is limited to
	// five small menu glyphs on the V1.
	int halo_side = target_side + 6;
	int halo_w = LV_MAX(1, source_w * halo_side / max_side);
	int halo_h = LV_MAX(1, source_h * halo_side / max_side);
	uint32_t halo_scale = (uint32_t)(LV_SCALE_NONE * halo_side / max_side);
	if (halo_scale == 0) {
		halo_scale = 1;
	}
	lv_obj_set_size(halo, halo_w, halo_h);
	lv_image_set_inner_align(halo, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(halo, halo_scale);
	lv_obj_set_hidden(halo, !glow);

	// If a theme change moved the tile, refresh the echo's position after the
	// flex layout has had a chance to size the foreground image.
	lv_obj_update_layout(tile);
	lv_obj_align_to(halo, icon, LV_ALIGN_CENTER, 0, 0);
}

static bool gridpage_tokyo_icon(const char *label, const lv_image_dsc_t **icon, theme_semantic_t *tone) {
	// Function-matched monochrome icons: the bitmap menu illustrations remain
	// untouched in Dark/Light, while Tokyo Night uses the existing compact
	// glyphs and a restrained semantic accent per destination.
	static const struct {
		const char *label;
		const lv_image_dsc_t *icon;
		theme_semantic_t tone;
	} icons[] = {
		{"music", &icon_music2, THEME_SEMANTIC_CYAN},
		{"streaming", &icon_radio_player, THEME_SEMANTIC_PURPLE},
		{"wireless", &icon_wifi_high, THEME_SEMANTIC_BLUE},
		{"audiobooks", &icon_files_audiobook, THEME_SEMANTIC_AMBER},
		{"more", &icon_ellipsis_vertical, THEME_SEMANTIC_GREEN},
		{"settings", &icon_music_settings, THEME_SEMANTIC_MUTED},
		{"music_all_tracks", &icon_list_music, THEME_SEMANTIC_CYAN},
		{"albums", &icon_album, THEME_SEMANTIC_PURPLE},
		{"artists", &icon_artist, THEME_SEMANTIC_GREEN},
		{"music_album_artists", &icon_artist_album, THEME_SEMANTIC_BLUE},
		{"music_genres", &icon_genre, THEME_SEMANTIC_AMBER},
		{"music_years", &icon_year, THEME_SEMANTIC_AMBER},
		{"music_most_played", &icon_most_played, THEME_SEMANTIC_PURPLE},
		{"favourites", &icon_star, THEME_SEMANTIC_AMBER},
		{"music_browse", &icon_folder, THEME_SEMANTIC_MUTED},
		{"playlists", &icon_list_music, THEME_SEMANTIC_CYAN},
		{"wi_fi", &icon_wifi, THEME_SEMANTIC_BLUE},
		{"bluetooth", &icon_bluetooth, THEME_SEMANTIC_PURPLE},
		{"airplay", &icon_airplay_quick, THEME_SEMANTIC_CYAN},
		{"transfer", &icon_wifi_transfer_quick, THEME_SEMANTIC_GREEN},
		{"sonixlink", &icon_sonixlink_quick, THEME_SEMANTIC_AMBER},
		{"dlna", &icon_dlna_quick, THEME_SEMANTIC_BLUE},
		{"tidal", &icon_tidal_mark, THEME_SEMANTIC_CYAN},
		{"qobuz", &icon_qobuz_mark, THEME_SEMANTIC_PURPLE},
		{"subsonic", &icon_music_note, THEME_SEMANTIC_BLUE},
		{"radio", &icon_radio_player, THEME_SEMANTIC_PURPLE},
		{"podcasts", &icon_podcast_list, THEME_SEMANTIC_CYAN},
		{"audiobook_library", &icon_book_headphones, THEME_SEMANTIC_AMBER},
		{"audiobook_series", &icon_book_finished, THEME_SEMANTIC_PURPLE},
		{"audiobook_authors", &icon_artist, THEME_SEMANTIC_BLUE},
		{"audiobook_continue", &icon_play, THEME_SEMANTIC_GREEN},
		{"dac", &icon_usbaudioout, THEME_SEMANTIC_CYAN},
		{"file_explorer", &icon_folder, THEME_SEMANTIC_BLUE},
	};

	if (!label || !icon || !tone) {
		return false;
	}
	for (unsigned int i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
		if (strcmp(label, icons[i].label) == 0) {
			*icon = icons[i].icon;
			*tone = icons[i].tone;
			return true;
		}
	}
	return false;
}

static void gridpage_apply_theme_page(gridpage_theme_page_t *page) {
	if (!page || !page->grid) {
		return;
	}

	bool tokyo = theme_uses_function_glyphs();
	for (int i = 0; i < page->count; i++) {
		const lv_image_dsc_t *icon = page->normal_icons[i];
		theme_semantic_t tone = THEME_SEMANTIC_BLUE;
		bool keep = page->keep_color[i];
		if (tokyo && !keep) {
			gridpage_tokyo_icon(page->labels[i], &icon, &tone);
		}
		// A colour icon is drawn as it came, at the classic size; only the
		// monochrome menu glyphs take the layout's accent and compact size.
		bool tinted = tokyo && !keep;
		bool horizontal = tinted && compact_grid();
		gridpage_set_tile_icon_style(page->grid, i, icon,
									 tinted ? (compact_grid() ? 22 : 42)
											: (compact_grid() ? COMPACT_TILE_ICON_MAX : 0),
									 tinted, theme_semantic_color(tone), false);
		gridpage_set_tile_orientation(page->grid, i, horizontal);

		lv_obj_t *tile = lv_obj_get_child(page->grid, i);
		if (tile) {
			// Tokyo Night tiles are separated by gaps and fill, not keylines.
			// Clear both border and focus-outline styles; a zero-opacity line
			// still reads as a card edge on the V1's small panel.
			lv_obj_set_style_border_width(tile, 0, 0);
			if (tokyo) {
				lv_obj_set_style_outline_width(tile, 0, 0);
			} else {
				lv_obj_remove_local_style_prop(tile, LV_STYLE_OUTLINE_WIDTH, 0);
			}
		}
	}
}

static void gridpage_refresh_theme(void) {
	for (int i = 0; i < theme_page_count; i++) {
		gridpage_apply_theme_page(&theme_pages[i]);
	}
}

static gridpage_theme_page_t *gridpage_theme_page_for(lv_obj_t *grid) {
	for (int i = 0; i < theme_page_count; i++) {
		if (theme_pages[i].grid == grid) {
			return &theme_pages[i];
		}
	}
	return NULL;
}

static void gridpage_register_theme_page(lv_obj_t *grid, const grid_entry_t *entries, int count) {
	if (!grid) {
		return;
	}

	gridpage_theme_page_t *page = gridpage_theme_page_for(grid);
	if (!entries || count < 0 || count > GRIDPAGE_THEME_TILE_MAX) {
		// More tiles than the page table holds. Drop the old registration rather
		// than leave it naming icons the caller may free.
		if (page) {
			page->count = 0;
		}
		return;
	}

	if (!page) {
		if (theme_page_count >= GRIDPAGE_THEME_PAGE_MAX) {
			return;
		}
		page = &theme_pages[theme_page_count++];
		page->grid = grid;
	}
	page->count = count;
	for (int i = 0; i < count; i++) {
		page->normal_icons[i] = entries[i].icon;
		page->labels[i] = entries[i].label;
		page->keep_color[i] = entries[i].keep_color;
	}
	if (!theme_refresh_registered) {
		theme_register_refresh(gridpage_refresh_theme);
		theme_refresh_registered = true;
	}
	gridpage_apply_theme_page(page);
}

void gridpage_set_tile_orientation(lv_obj_t *grid, int index, bool horizontal) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}

	lv_obj_t *tile = lv_obj_get_child(grid, index);
	if (!tile) {
		return;
	}
	lv_obj_t *icon = tile_icon_object(tile);
	lv_obj_t *label = tile_label_object(tile);
	if (!icon || !label) {
		return;
	}

	bool compact = compact_grid();
	int padding = horizontal ? bp_pick(4, 8) : (compact ? 3 : 8);
	int gap = horizontal ? bp_pick(6, 8) : (compact ? 2 : 8);
	lv_obj_set_style_pad_all(tile, padding, 0);
	lv_obj_set_style_pad_gap(tile, gap, 0);
	lv_obj_set_flex_flow(tile, horizontal ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, horizontal ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
					  LV_FLEX_ALIGN_CENTER);

	if (horizontal) {
		int label_width = lv_obj_get_width(tile) - 2 * padding - lv_obj_get_width(icon) - gap;
		lv_obj_set_width(label, LV_MAX(1, label_width));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
	} else {
		lv_obj_set_width(label, lv_pct(100));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	}

	lv_obj_update_layout(tile);
	if (tile_has_icon_halo(tile)) {
		lv_obj_align_to(lv_obj_get_child(tile, 0), icon, LV_ALIGN_CENTER, 0, 0);
	}
}

void gridpage_set_tile_label_hidden(lv_obj_t *grid, int index, bool hidden) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}

	lv_obj_t *label = tile_label_object(lv_obj_get_child(grid, index));
	if (label) {
		lv_obj_set_hidden(label, hidden);
	}
}

void gridpage_set_tile_label_lines(lv_obj_t *grid, int index, int lines) {
	if (!grid || index < 0 || index >= (int)lv_obj_get_child_count(grid)) {
		return;
	}

	lv_obj_t *tile = lv_obj_get_child(grid, index);
	lv_obj_t *label = tile_label_object(tile);
	if (!label) {
		return;
	}

	lines = LV_CLAMP(lines, 1, 3);
	const lv_font_t *font = compact_grid() ? bp_tile_label_font() : &font_ui_24_bold;
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_line_space(label, 0, 0);
	lv_obj_set_height(label, lv_font_get_line_height(font) * lines);
	lv_obj_update_layout(tile);
	if (tile_has_icon_halo(tile)) {
		lv_obj_align_to(lv_obj_get_child(tile, 0), tile_icon_object(tile), LV_ALIGN_CENTER, 0, 0);
	}
}

void gridpage_set_wide_first(lv_obj_t *grid, bool wide) {
	gridpage_theme_page_t *page = gridpage_theme_page_for(grid);
	if (page) {
		page->wide_first = wide;
	}
}

void gridpage_set_layout(lv_obj_t *grid, gui_config_t *cfg, int top, int bottom, int padding, int gap, int columns,
						 int rows) {
	if (!grid || !cfg || columns < 1 || rows < 1 || top < 0 || bottom < 0 || top + bottom >= (int)cfg->screen_height) {
		return;
	}

	int grid_height = (int)cfg->screen_height - top - bottom;
	lv_obj_set_size(grid, lv_pct(100), grid_height);
	lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_all(grid, padding, 0);
	lv_obj_set_style_pad_gap(grid, gap, 0);

	int usable_w = (int)cfg->screen_width - 2 * padding;
	int usable_h = grid_height - 2 * padding;
	if (usable_w <= 0 || usable_h <= 0) {
		return;
	}
	int tile_w = (usable_w - (columns - 1) * gap) / columns;
	int tile_h = (usable_h - (rows - 1) * gap) / rows;
	if (tile_w <= 0 || tile_h <= 0) {
		return;
	}

	gridpage_theme_page_t *page = gridpage_theme_page_for(grid);
	bool wide_first = page && page->wide_first;
	int count = (int)lv_obj_get_child_count(grid);
	for (int i = 0; i < count; i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		bool lone_last = !wide_first && count == columns * rows - 1 && i == count - 1;
		bool wide = (wide_first && i == 0) || lone_last;
		lv_obj_set_size(tile, wide ? usable_w : tile_w, tile_h);
	}
	lv_obj_update_layout(grid);

	// A halo is a floating child, so the layout ignores it; explicitly follow
	// its tile icon after the grid has moved or changed size.
	for (int i = 0; i < count; i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		if (!tile_has_icon_halo(tile)) {
			continue;
		}
		lv_obj_t *halo = lv_obj_get_child(tile, 0);
		lv_obj_t *icon = tile_icon_object(tile);
		if (halo && icon) {
			lv_obj_align_to(halo, icon, LV_ALIGN_CENTER, 0, 0);
		}
	}
}

static void add_tile(lv_obj_t *grid, const grid_entry_t *entry, int width, int height) {
	const bool compact = compact_grid();
	lv_obj_t *tile = lv_btn_create(grid);
	lv_obj_set_size(tile, width, height);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, bp_tile_radius(), 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, compact ? 3 : 8, 0);

	// Presses bubble up to the grid so a swipe can start on a tile: the whole
	// page has to be draggable, not just the gaps between the tiles.
	lv_obj_set_event_bubble(tile, true);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, compact ? 2 : 8, 0);
	lv_obj_set_scrollable(tile, false);

	// The source menu art is 112-128 px.  On a compact three-row page an entire
	// tile can be only about 75 px high, so both its drawing and its layout box
	// have to shrink.  lv_image_set_scale() changes only the drawing: leaving
	// the object's intrinsic 128 px box in flex layout pushes the caption below
	// the tile even though the visible icon looks small.  This explicit box is
	// what keeps the caption in the card on the V1.
	lv_obj_t *icon = lv_image_create(tile);
	lv_image_set_src(icon, entry->icon);
	if (compact && entry->icon) {
		int source_w = (int)entry->icon->header.w;
		int source_h = (int)entry->icon->header.h;
		int max_side = LV_MAX(source_w, source_h);
		if (max_side > COMPACT_TILE_ICON_MAX) {
			int icon_w = LV_MAX(1, source_w * COMPACT_TILE_ICON_MAX / max_side);
			int icon_h = LV_MAX(1, source_h * COMPACT_TILE_ICON_MAX / max_side);
			lv_obj_set_size(icon, icon_w, icon_h);
			lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * COMPACT_TILE_ICON_MAX / max_side));
		}
	}

	// Compact captions are deliberately one line with an ellipsis.  A wrapped
	// French or German caption used to put the second line's descenders outside
	// the short V1 tile.  Pinning the height and zeroing line spacing gives
	// LV_LABEL_LONG_DOT an exact clipping boundary.
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text(label, tr(entry->label));
	lv_obj_add_style(label, &theme_style_text, 0);
	const lv_font_t *label_font = compact ? bp_tile_label_font() : &font_ui_24_bold;
	lv_obj_set_style_text_font(label, label_font, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	if (compact) {
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_line_space(label, 0, 0);
		lv_obj_set_height(label, lv_font_get_line_height(label_font));
	}

	if (entry->target) {
		lv_obj_add_event_cb(tile, switch_screen_cb, LV_EVENT_CLICKED, *entry->target);
	} else if (entry->action) {
		lv_obj_add_event_cb(tile, action_cb, LV_EVENT_CLICKED, (void *)entry->action);
	} else if (entry->action_user) {
		lv_obj_add_event_cb(tile, action_user_cb, LV_EVENT_CLICKED, (void *)entry);
	} else {
		lv_obj_add_event_cb(tile, unavailable_cb, LV_EVENT_CLICKED, (void *)entry->label);
		lv_obj_set_style_opa(icon, LV_OPA_40, 0);
		lv_obj_add_style(label, &theme_style_text_dim, 0);
	}
}

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
						 int rows, bool clear_corner_buttons) {
	lv_obj_add_style(screen, &theme_style_screen, 0);

	int top = clear_corner_buttons ? settingsrow_content_top(cfg) : cfg->top_bar_height;

	lv_obj_t *grid = lv_obj_create(screen);
	lv_obj_set_size(grid, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(grid, 0, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_radius(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, cfg->padding, 0);
	int gap = grid_gap();
	lv_obj_set_style_pad_gap(grid, gap, 0);
	lv_obj_set_scrollable(grid, false);

	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis, not CENTER: the tile size is computed for a full
	// `columns` x `rows` grid, so a page with fewer entries reads as that grid
	// with empty places. Centring would drift an odd tile into the middle of
	// its row, giving that page a shape no other one has.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	int usable_w = cfg->screen_width - (2 * cfg->padding);
	int usable_h = (cfg->screen_height - top) - (2 * cfg->padding);

	int tile_w = (usable_w - (columns - 1) * gap) / columns;
	int tile_h = (usable_h - (rows - 1) * gap) / rows;

	if (count > columns * rows) {
		lv_obj_set_scrollable(grid, true);
		lv_obj_set_scroll_dir(grid, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
	}

	for (int i = 0; i < count; i++) {
		// Five entries in a 2x3 page used to leave a conspicuous dead card-sized
		// hole at bottom right.  Let the final destination span that row; pages
		// with four entries still intentionally leave their third row empty.
		bool lone_last = (count == columns * rows - 1 || (count > columns * rows && count % columns == 1)) &&
						 i == count - 1;
		add_tile(grid, &entries[i], lone_last ? usable_w : tile_w, tile_h);
	}

	// The player can be pulled in from any tiled page.
	player_sheet_attach_drag(grid, true);
	// Presses die here (the tiles bubble only one level up), so the grid must
	// carry the swipe-back drag itself, like the lists do.
	switcher_attach_back_gesture(grid);

	// Handed back for the same reason: a caller that wants a gesture of its own
	// on the page has to put it on this object, because nothing below it
	// bubbles any further.
	gridpage_register_theme_page(grid, entries, count);
	return grid;
}

void gridpage_replace_entries(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
							  int rows) {
	if (!grid || !cfg || !entries || columns < 1 || rows < 1) {
		return;
	}

	lv_obj_clean(grid);
	int gap = grid_gap();
	int top = settingsrow_content_top(cfg);
	int usable_w = (int)cfg->screen_width - (2 * (int)cfg->padding);
	int usable_h = ((int)cfg->screen_height - top) - (2 * (int)cfg->padding);
	int tile_w = (usable_w - (columns - 1) * gap) / columns;
	int tile_h = (usable_h - (rows - 1) * gap) / rows;

	if (count > columns * rows) {
		lv_obj_set_scrollable(grid, true);
		lv_obj_set_scroll_dir(grid, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
	} else {
		lv_obj_set_scrollable(grid, false);
		lv_obj_scroll_to_y(grid, 0, LV_ANIM_OFF);
	}

	gridpage_theme_page_t *page = gridpage_theme_page_for(grid);
	bool wide_first = page && page->wide_first;
	for (int i = 0; i < count; i++) {
		bool lone_last = !wide_first &&
						 (count == columns * rows - 1 || (count > columns * rows && count % columns == 1)) &&
						 i == count - 1;
		bool wide = (wide_first && i == 0) || lone_last;
		add_tile(grid, &entries[i], wide ? usable_w : tile_w, tile_h);
	}
	gridpage_register_theme_page(grid, entries, count);
	lv_obj_update_layout(grid);
}

lv_obj_t *gridpage_empty_panel(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *icon, const char *text,
							   lv_event_cb_t scan_cb) {
	const bool compact = compact_grid();
	int top = settingsrow_content_top(cfg);

	lv_obj_t *panel = lv_obj_create(screen);
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_bottom(panel, cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(panel, bp_pick(8, 22), 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(panel, true);

	// The source art is 128 px, which is more than half of the V1's 236 px of
	// content height on its own.  As on the tiles, the drawing *and* the
	// layout box both have to shrink: lv_image_set_scale() alone would leave a
	// 128 px box in the flex column and push the button off the bottom of the
	// panel, which is exactly what the full-size geometry used to do here.
	lv_obj_t *picture = lv_image_create(panel);
	lv_image_set_src(picture, icon);
	lv_obj_add_style(picture, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(picture, LV_OPA_COVER, 0);
	if (compact && icon) {
		int source_w = (int)icon->header.w;
		int source_h = (int)icon->header.h;
		int max_side = LV_MAX(source_w, source_h);
		if (max_side > COMPACT_EMPTY_ICON_MAX) {
			int icon_w = LV_MAX(1, source_w * COMPACT_EMPTY_ICON_MAX / max_side);
			int icon_h = LV_MAX(1, source_h * COMPACT_EMPTY_ICON_MAX / max_side);
			lv_obj_set_size(picture, icon_w, icon_h);
			lv_image_set_inner_align(picture, LV_IMAGE_ALIGN_CENTER);
			lv_image_set_scale(picture, (uint32_t)(LV_SCALE_NONE * COMPACT_EMPTY_ICON_MAX / max_side));
		}
	}

	lv_obj_t *label = lv_label_create(panel);
	lv_label_set_text(label, tr(text));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	// One step down from the regular size on the V1: this sentence is the
	// longest piece of running text in the shell, and a translation of it has
	// to be able to wrap to six lines and still leave room for the button.
	lv_obj_set_style_text_font(label, compact ? &font_ui_20 : &font_ui_24, 0);

	// 240 px is the full width of the V1 panel, so the regular button was also
	// wider than the padded column it sits in.
	lv_obj_t *button = lv_btn_create(panel);
	lv_obj_set_size(button, bp_pick(132, 240), bp_pick(38, 68));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, theme()->accent, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, scan_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *button_label = lv_label_create(button);
	lv_label_set_text(button_label, tr("scan"));
	lv_obj_set_style_text_font(button_label, compact ? &font_ui_20 : &font_ui_24, 0);
	lv_obj_set_style_text_color(button_label, theme_on_accent(), 0);
	lv_obj_center(button_label);

	return panel;
}

void gridpage_show_empty(lv_obj_t *grid, lv_obj_t *panel, bool empty) {
	if (grid) {
		if (empty) {
			lv_obj_set_hidden(grid, true);
		} else {
			lv_obj_set_hidden(grid, false);
		}
	}
	if (panel) {
		if (empty) {
			// The accent may have changed since the panel was built.
			lv_obj_t *button = lv_obj_get_child(panel, 2);
			lv_obj_set_style_bg_color(button, theme()->accent, 0);
			lv_obj_set_style_text_color(lv_obj_get_child(button, 0), theme_on_accent(), 0);
			lv_obj_set_hidden(panel, false);
		} else {
			lv_obj_set_hidden(panel, true);
		}
	}
}

void gridpage_empty_set_text(lv_obj_t *panel, const char *text) {
	if (!panel || !text) {
		return;
	}
	// gridpage_empty_panel() builds the picture, then the sentence, then the
	// button; the sentence is the label that is not inside the button.
	lv_obj_t *label = lv_obj_get_child(panel, 1);
	if (label && lv_obj_check_type(label, &lv_label_class)) {
		lv_label_set_text(label, tr(text));
	}
}
