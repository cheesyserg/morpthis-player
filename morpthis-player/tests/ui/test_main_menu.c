/* Real LVGL, menus, appearance controls, themes, translations and persistence.
 * Only fonts and unrelated playback/navigation/board services are substituted. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/main_menu_icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *streaming_screen, *wireless_screen;
lv_obj_t *audiobooks_screen, *morepage_screen, *settings_screen, *player_screen;
lv_obj_t *browser_screen, *musicsettings_screen;

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *fonts[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
		&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
		&font_ui_26, &font_ui_28, &font_ui_32,
	};
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		*fonts[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) {
	assert(font_callback_count < 16);
	font_callbacks[font_callback_count++] = cb;
}
int fonts_text_size(void) { return text_size; }
bool fonts_set_text_size(int size) {
	text_size = size;
	/* Deliberately enlarge the footer enough to force the grid to reflow. */
	font_ui_14.line_height = size == FONTS_TEXT_LARGE ? 24 : 14;
	for (int i = 0; i < font_callback_count; i++) {
		font_callbacks[i]();
	}
	return true;
}
const char *fonts_choice(void) { return "default"; }
bool fonts_set_face(const char *id) { return strcmp(id, "default") == 0; }
int fonts_choices(const char **ids, const char **labels, int max) {
	assert(max > 0);
	ids[0] = "default";
	labels[0] = "Test font";
	return 1;
}

void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
lv_obj_t *bootscreen_screen;
const char *bootscreen_applied_name(void) { return "Retrospace"; }
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"unexpected missing card"); }
void gui_notify_popup(const char *text) { (void)text; assert(!"unexpected popup"); }
void toast_error(const char *text) { (void)text; assert(!"unexpected error"); }
void topbar_set_battery_percent(bool shown) { (void)shown; }
void topbar_set_clock_position(int pos) { (void)pos; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
static bool browser_music_started;
void browser_start_in_music_folder(void) { browser_music_started = true; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool player_sheet_drag_active(void) { return false; }
void player_refresh_now_playing(void) {}
void player_sheet_open(bool animated) { (void)animated; }
void coverflow_attach_edge(lv_obj_t *obj, gui_config_t *cfg) { (void)obj; (void)cfg; }
bool coverflow_drag_active(void) { return false; }
void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *value) {
	(void)title; (void)kind; (void)filter; (void)value;
}
void medialist_open_years(void) {}
void medialist_open_most_played(void) {}
void playlistpage_open(void) {}
void search_open(void) {}
void libraryscan_choose_folders(void) {}
bool library_scan_running(void) { return false; }
int library_track_count(void) { return 42; }
bool library_track_names(const char *path, char *title, size_t title_len, char *artist, size_t artist_len) {
	(void)path; (void)title; (void)title_len; (void)artist; (void)artist_len;
	return false;
}
int playlist_count(void) { return 0; }
int playlist_current_index(void) { return -1; }
unsigned playlist_revision(void) { return 0; }
bool playlist_path_at(int index, char *out, size_t size) { (void)index; (void)out; (void)size; return false; }
bool audiobookdb_book_for_file(const char *path, char *book_out, size_t size) {
	(void)path; (void)book_out; (void)size;
	return false;
}
bool device_state_play_queue_index(int index) { (void)index; return false; }
void device_state_play_file(const char *path) { (void)path; }
bool musicsettings_playlists_first(void) { return false; }

static music_tile_id_t test_active_tiles[MUSIC_TILE_COUNT] = {
	MUSIC_TILE_ALL_TRACKS, MUSIC_TILE_ALBUMS, MUSIC_TILE_ARTISTS,
	MUSIC_TILE_ALBUM_ARTISTS, MUSIC_TILE_GENRES, MUSIC_TILE_YEARS,
	MUSIC_TILE_PLAYLISTS, MUSIC_TILE_MOST_PLAYED, MUSIC_TILE_BROWSE,
};
static int test_active_tile_count = MUSIC_TILE_COUNT;

int musicsettings_main_tiles(music_tile_id_t *out, int max) {
	int n = test_active_tile_count < max ? test_active_tile_count : max;
	if (out && n > 0) {
		memcpy(out, test_active_tiles, (size_t)n * sizeof(music_tile_id_t));
	}
	return n;
}

static lv_obj_t *child_of_type(lv_obj_t *obj, const lv_obj_class_t *type) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *child = lv_obj_get_child(obj, i);
		if (lv_obj_check_type(child, type) && !lv_obj_is_floating(child)) {
			return child;
		}
	}
	assert(!"missing child");
	return NULL;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/* Optional real-renderer diagnostics, kept out of Git. No display driver or
 * image library is needed; PPM can be opened by ordinary image tools. */
static void dump_menu_snapshot(int kind) {
	const char *prefix = getenv("MORPTHIS_MENU_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_draw_buf_t *shot = lv_snapshot_take(main_menu_screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%d-theme%d.ppm", prefix, bp_screen_w(), kind);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

static lv_obj_t *menu_toggle;
static void set_icons_only(bool enabled) {
	lv_obj_set_state(menu_toggle, LV_STATE_CHECKED, enabled);
	lv_obj_send_event(menu_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_icons_only() == enabled);
}

/* Layout-only tests missed the previous blur: the box was the right size,
 * but LVGL still enlarged a tiny antialiased toolbar bitmap inside it. Verify
 * the real software renderer preserves the new source's pixels one-for-one,
 * including its alpha edges and theme tint, in ARGB8888 and the panel's RGB565. */
static void check_native_icon(lv_obj_t *icon, int tile, int size) {
	const lv_image_dsc_t *source = lv_image_get_src(icon);
	assert(source == main_menu_icon_native(tile, theme_uses_function_glyphs(), size));
	assert(source && source->header.w == size && source->header.h == size);
	assert(lv_image_get_scale_x(icon) == LV_SCALE_NONE);
	assert(lv_image_get_scale_y(icon) == LV_SCALE_NONE);
	assert(lv_image_get_rotation(icon) == 0);
	bool mask = source->header.cf == LV_COLOR_FORMAT_A8;
	assert(mask == theme_uses_function_glyphs());
	lv_color_t tint = lv_obj_get_style_image_recolor(icon, 0);
	lv_draw_buf_t *rgba = lv_snapshot_take(icon, LV_COLOR_FORMAT_ARGB8888);
	lv_draw_buf_t *rgb565 = lv_snapshot_take(icon, LV_COLOR_FORMAT_RGB565);
	assert(rgba && rgb565);
	assert(rgba->header.w == size && rgba->header.h == size);
	assert(rgb565->header.w == size && rgb565->header.h == size);
	int opaque_pixels = 0;
	for (int y = 0; y < size; y++) {
		const uint8_t *src = source->data + y * source->header.stride;
		const uint8_t *out = rgba->data + y * rgba->header.stride;
		const uint8_t *panel = rgb565->data + y * rgb565->header.stride;
		for (int x = 0; x < size; x++) {
			int alpha = mask ? src[x] : src[x * 4 + 3];
			/* LVGL treats near-transparent/opaque values as 0/255. Any
			 * interpolation, half-pixel shift or lost A8 mask fails this. */
			assert(abs(out[x * 4 + 3] - alpha) <= 3);
			if (alpha != 255) {
				continue;
			}
			opaque_pixels++;
			uint8_t r = mask ? tint.red : src[x * 4 + 2];
			uint8_t g = mask ? tint.green : src[x * 4 + 1];
			uint8_t b = mask ? tint.blue : src[x * 4];
			assert(out[x * 4] == b && out[x * 4 + 1] == g && out[x * 4 + 2] == r);
			uint16_t actual;
			memcpy(&actual, panel + x * 2, sizeof(actual));
			uint16_t expected = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
			assert(actual == expected);
		}
	}
	assert(opaque_pixels > 0);
	lv_draw_buf_destroy(rgba);
	lv_draw_buf_destroy(rgb565);
}

static void check_native_assets(void) {
	for (int tile = 0; tile < 6; tile++) {
		for (int size = 1; size <= 173; size++) {
			const lv_image_dsc_t *glyph = main_menu_icon_native(tile, true, size);
			const lv_image_dsc_t *color = main_menu_icon_native(tile, false, size);
			assert((glyph != NULL) == (size <= 47 || size == 138 || size == 173));
			assert((color != NULL) == (size == 74 || size == 173));
			const lv_image_dsc_t *sources[] = {glyph, color};
			for (size_t i = 0; i < 2; i++) {
				const lv_image_dsc_t *src = sources[i];
				if (!src) {
					continue;
				}
				assert(src->header.magic == LV_IMAGE_HEADER_MAGIC);
				assert(src->header.w == size && src->header.h == size);
				assert(src->header.cf == (i == 0 ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888));
				assert(src->header.stride % 4 == 0);
				assert(src->data_size == src->header.stride * src->header.h);
			}
		}
	}
	assert(!main_menu_icon_native(-1, true, 47));
	assert(!main_menu_icon_native(6, false, 74));
	assert(!main_menu_icon_native(0, true, -1));
	assert(!main_menu_icon_native(0, true, 0));
	assert(!main_menu_icon_native(0, false, 174));
}

static void check_grid(lv_obj_t *grid, bool icons_only) {
	lv_obj_update_layout(grid);
	for (uint32_t i = 0; i < lv_obj_get_child_count(grid); i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		lv_obj_t *label = child_of_type(tile, &lv_label_class);
		assert(lv_obj_is_hidden(label) == icons_only);
		assert(lv_label_get_text(label)[0]);
		if (lv_obj_get_parent(grid) == main_menu_screen) {
			/* Icon-only tiles must stay sharp: no visible halo, even if it
			 * already exists from the labeled layout or a previous theme. */
			int visible_halos = 0;
			for (uint32_t j = 0; j < lv_obj_get_child_count(tile); j++) {
				lv_obj_t *child = lv_obj_get_child(tile, j);
				if (lv_obj_is_floating(child) && !lv_obj_is_hidden(child)) {
					visible_halos++;
				}
			}
			bool glow_expected = !icons_only && bp_is_tempotec_v1() && theme_uses_function_glyphs() && (i % 2 == 0);
			assert(visible_halos == (glow_expected ? 1 : 0));
		}
		if (!icons_only) {
			continue;
		}
		lv_obj_t *icon = child_of_type(tile, &lv_image_class);
		int long_side = LV_MAX(lv_obj_get_width(icon), lv_obj_get_height(icon));
		int default_size = theme_uses_function_glyphs() ? bp_pick(22, 46) : bp_pick(46, 128);
		assert(long_side > default_size);
		/* Preserve OLED's earlier reduction, then shrink every theme's
		 * current icon-only size by 5%, rounded to the nearest pixel. */
		int available = LV_MAX(1, LV_MIN(lv_obj_get_width(tile), lv_obj_get_height(tile)) - bp_pick(12, 24));
		int previous_size = theme_is_oled() ? LV_MAX(1, available * 80 / 100) : available;
		int expected_size = LV_MAX(1, (previous_size * 95 + 50) / 100);
		assert(long_side == expected_size);
		check_native_icon(icon, (int)i, expected_size);
		lv_area_t t, image;
		lv_obj_get_coords(tile, &t);
		lv_obj_get_coords(icon, &image);
		assert(abs((t.x1 + t.x2) - (image.x1 + image.x2)) <= 2);
		assert(abs((t.y1 + t.y2) - (image.y1 + image.y2)) <= 2);
		assert(image.x1 >= t.x1 && image.x2 <= t.x2);
		assert(image.y1 >= t.y1 && image.y2 <= t.y2);
	}
}

typedef struct {
	int tile_w, tile_h, icon_w, icon_h, icon_x, icon_y;
	uint32_t scale;
	const void *source;
} tile_metrics_t;
static tile_metrics_t tile_metrics(lv_obj_t *tile) {
	lv_obj_update_layout(tile);
	lv_obj_t *icon = child_of_type(tile, &lv_image_class);
	return (tile_metrics_t){lv_obj_get_width(tile), lv_obj_get_height(tile),
		lv_obj_get_width(icon), lv_obj_get_height(icon), lv_obj_get_x(icon), lv_obj_get_y(icon),
		lv_image_get_scale(icon), lv_image_get_src(icon)};
}
static void check_metrics(lv_obj_t *tile, tile_metrics_t expected) {
	tile_metrics_t now = tile_metrics(tile);
	assert(now.tile_w == expected.tile_w && now.tile_h == expected.tile_h);
	assert(now.icon_w == expected.icon_w && now.icon_h == expected.icon_h);
	assert(now.icon_x == expected.icon_x && now.icon_y == expected.icon_y);
	assert(now.scale == expected.scale && now.source == expected.source);
}

int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	bool saved_icons_only = argc == 3;
	char dir[] = "/tmp/morpthis-menu-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config);
	assert(!main_menu_icons_only()); // no key: preserve the existing menu
	if (saved_icons_only) {
		config_set_bool("ui", "main_menu_icons_only", true);
		assert(config_save());
		assert(config_init(config));
	}
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	assert(strcmp(tr("appearance_main_menu_icons_only"), "Main menu icons only") == 0);
	init_fonts();
	lv_init();
	check_native_assets();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	browser_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	main_menu_screen = lv_obj_create(NULL);
	main_menu_init(&cfg);
	lv_screen_load(main_menu_screen);
	lv_obj_t *grid = lv_obj_get_child(main_menu_screen, 0);
	check_grid(grid, saved_icons_only);

	appearance_screen = lv_obj_create(NULL);
	appearance_init(&cfg);
	lv_obj_t *toggle_label = find_label(appearance_screen, tr("appearance_main_menu_icons_only"));
	assert(toggle_label);
	menu_toggle = child_of_type(lv_obj_get_parent(toggle_label), &lv_switch_class);
	assert(lv_obj_has_state(menu_toggle, LV_STATE_CHECKED) == saved_icons_only);

	assert(find_label(lv_obj_get_child(grid, 3), tr("audiobooks")));

	assert(!main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music")));
	lv_obj_t *browse_label = find_label(appearance_screen, tr("appearance_home_browse"));
	assert(browse_label);
	lv_obj_t *browse_toggle = child_of_type(lv_obj_get_parent(browse_label), &lv_switch_class);
	assert(!lv_obj_has_state(browse_toggle, LV_STATE_CHECKED));
	lv_obj_set_state(browse_toggle, LV_STATE_CHECKED, true);
	lv_obj_send_event(browse_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music_browse")));
	lv_obj_send_event(lv_obj_get_child(grid, 0), LV_EVENT_CLICKED, NULL);
	assert(browser_music_started);
	assert(lv_screen_active() == browser_screen);
	lv_screen_load(main_menu_screen);
	lv_obj_set_state(browse_toggle, LV_STATE_CHECKED, false);
	lv_obj_send_event(browse_toggle, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!main_menu_browse());
	assert(find_label(lv_obj_get_child(grid, 0), tr("music")));

	assert(icon_year.header.w == 32 && icon_year.header.h == 32 && icon_year.header.stride == 128);
	assert(icon_most_played.header.w == 32 && icon_most_played.header.h == 32 && icon_most_played.header.stride == 128);
	assert(icon_menu_year.header.w == 112 && icon_menu_year.header.h == 112 && icon_menu_year.header.stride == 448);
	assert(icon_menu_most_played.header.w == 112 && icon_menu_most_played.header.h == 112 && icon_menu_most_played.header.stride == 448);

	lv_obj_t *val_label = NULL;
	lv_obj_t *test_slider = NULL;
	lv_obj_t *slider_card = settingsrow_slider(settings_screen, "settings_screen_off", 5, &val_label, &test_slider, NULL);
	lv_obj_set_width(slider_card, cfg.screen_width - cfg.padding * 2);
	lv_obj_update_layout(settings_screen);
	lv_area_t card_area, slider_area;
	lv_obj_get_coords(slider_card, &card_area);
	lv_obj_get_coords(test_slider, &slider_area);
	int32_t knob_overhang = lv_obj_get_height(test_slider) / 2 + lv_obj_get_style_pad_right(test_slider, LV_PART_KNOB) + 2;
	assert(slider_area.x1 - knob_overhang >= card_area.x1 + 1);
	assert(slider_area.x2 + knob_overhang <= card_area.x2 - 1);

	musicsettings_screen = lv_obj_create(NULL);
	music_init(&cfg);
	lv_obj_t *music_tile_grid = lv_obj_get_child(music_screen, 0);
	assert(lv_obj_get_child_count(music_tile_grid) == MUSIC_TILE_COUNT);
	assert(find_label(music_tile_grid, tr("music_years")));
	assert(find_label(music_tile_grid, tr("music_most_played")));
	if (bp_is_tempotec_v1()) {
		lv_obj_t *tokyo_panel = lv_obj_get_child(music_screen, lv_obj_get_child_count(music_screen) - 1);
		assert(find_label(tokyo_panel, tr("music_years")));
		assert(find_label(tokyo_panel, tr("music_most_played")));
	}

	/* Rearrange and disable Years and Most played; verify both tile_grid and tokyo_music_panel update. */
	test_active_tiles[0] = MUSIC_TILE_PLAYLISTS;
	test_active_tiles[1] = MUSIC_TILE_ALBUMS;
	test_active_tiles[2] = MUSIC_TILE_ARTISTS;
	test_active_tiles[3] = MUSIC_TILE_ALL_TRACKS;
	test_active_tiles[4] = MUSIC_TILE_GENRES;
	test_active_tile_count = 5;
	music_refresh_layout();
	assert(lv_obj_get_child_count(music_tile_grid) == 5);
	assert(find_label(lv_obj_get_child(music_tile_grid, 0), tr("playlists")));
	assert(!find_label(music_tile_grid, tr("music_years")));
	assert(!find_label(music_tile_grid, tr("music_most_played")));
	if (bp_is_tempotec_v1()) {
		lv_obj_t *tokyo_panel = lv_obj_get_child(music_screen, lv_obj_get_child_count(music_screen) - 1);
		lv_obj_t *browse_card = lv_obj_get_child(tokyo_panel, 0);
		lv_obj_t *browse_grid = lv_obj_get_child(browse_card, 1);
		assert(find_label(lv_obj_get_child(browse_grid, 0), tr("playlists")));
		assert(!find_label(tokyo_panel, tr("music_years")));
		assert(!find_label(tokyo_panel, tr("music_most_played")));
		assert(!find_label(tokyo_panel, tr("music_album_artist")));
	}

	const grid_entry_t entries[] = {
		{"music_all_tracks", &icon_menu_music, &music_screen, NULL},
		{"albums", &icon_album, &music_screen, NULL},
		{"music_years", &icon_menu_year, &music_screen, NULL},
		{"music_most_played", &icon_menu_most_played, &music_screen, NULL},
		{"playlists", &icon_menu_playlist, &music_screen, NULL},
	};
	lv_obj_t *section_grid = gridpage_build(streaming_screen, &cfg, entries, 5, 2, 3, true);
	gridpage_set_tile_label_hidden(NULL, 0, true);
	gridpage_set_tile_label_hidden(section_grid, -1, true);
	gridpage_set_tile_label_hidden(section_grid, 99, true);

	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		set_icons_only(false);
		theme_set_kind((theme_kind_t)kind);
		tile_metrics_t year_tile = tile_metrics(lv_obj_get_child(section_grid, 2));
		tile_metrics_t most_tile = tile_metrics(lv_obj_get_child(section_grid, 3));
		tile_metrics_t play_tile = tile_metrics(lv_obj_get_child(section_grid, 4));
		if (theme_uses_function_glyphs()) {
			assert(year_tile.source == &icon_year);
			assert(most_tile.source == &icon_most_played);
			assert(play_tile.source == &icon_list_music);
		} else {
			assert(year_tile.source == &icon_menu_year);
			assert(most_tile.source == &icon_menu_most_played);
			assert(play_tile.source == &icon_menu_playlist);
		}
		tile_metrics_t baseline[6];
		for (int i = 0; i < 6; i++) {
			baseline[i] = tile_metrics(lv_obj_get_child(grid, i));
		}
		tile_metrics_t section_before = tile_metrics(lv_obj_get_child(section_grid, 0));
		bool header_hidden = lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 1));
		bool footer_hidden = lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 3));

		set_icons_only(true);
		assert(config_init(config)); // a real save/reload, not just the in-memory flag
		assert(main_menu_icons_only());
		check_grid(grid, true);
		check_grid(section_grid, false);
		check_metrics(lv_obj_get_child(section_grid, 0), section_before);
		assert(lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 1)) == header_hidden);
		assert(lv_obj_is_hidden(lv_obj_get_child(main_menu_screen, 3)) == footer_hidden);
		dump_menu_snapshot(kind);

		theme_set_accent(3);
		theme_set_dynamic_tint(true);
		check_grid(grid, true);
		check_grid(section_grid, false);
		fonts_set_text_size(FONTS_TEXT_LARGE);
		check_grid(grid, true);
		if (bp_is_tempotec_v1() && theme_uses_function_glyphs()) {
			/* Custom faces have different line metrics even at one text size.
			 * Exercise odd/even pixel sizes as the footer takes more space. */
			for (int line_height = 15; line_height <= 32; line_height++) {
				font_ui_14.line_height = line_height;
				for (int i = 0; i < font_callback_count; i++) {
					font_callbacks[i]();
				}
				check_grid(grid, true);
			}
		}
		fonts_set_text_size(FONTS_TEXT_NORMAL);
		check_grid(grid, true);

		set_icons_only(false);
		assert(config_init(config));
		assert(!main_menu_icons_only());
		check_grid(grid, false);
		for (int i = 0; i < 6; i++) {
			check_metrics(lv_obj_get_child(grid, i), baseline[i]);
		}
		/* Refresh callbacks must not undo the preference on a theme change. */
		set_icons_only(true);
		theme_set_kind((theme_kind_t)((kind + 1) % THEME_KIND_COUNT));
		check_grid(grid, true);
		check_grid(section_grid, false);
	}

	/* Hidden labels still translate, and all six destinations remain clickable. */
	assert(lang_set("Français"));
	check_grid(grid, true);
	lv_obj_t *music_label = child_of_type(lv_obj_get_child(grid, 0), &lv_label_class);
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	assert(lang_set("English"));
	assert(strcmp(lv_label_get_text(music_label), tr("music")) == 0);
	lv_obj_t *destinations[] = {music_screen, streaming_screen, wireless_screen,
		audiobooks_screen, morepage_screen, settings_screen};
	for (int i = 0; i < 6; i++) {
		lv_obj_send_event(lv_obj_get_child(grid, i), LV_EVENT_CLICKED, NULL);
		assert(lv_screen_active() == destinations[i]);
		lv_screen_load(main_menu_screen);
		check_grid(grid, true);
	}

	lv_deinit();
	unlink(config);
	rmdir(dir);
	printf("PASS main-menu icons-only (%s, %s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720",
		saved_icons_only ? "saved setting" : "default setting");
	return 0;
}
