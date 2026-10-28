/* The screensaver's Framed style, on the real LVGL renderer.
 *
 * The album source used to have one look: the player's sleeve -- decoded at
 * the sleeve's size, about 156 px a side on the V1 -- enlarged across the
 * screensaver's artwork area, which blurs it into blocks. Framed is the other
 * look, and what it promises is checked here on the rendered pixels: the
 * picture whole, one pixel to one, with margins and never enlarged; decoded for
 * exactly that frame, asked of the cover loader at the right moments, and let
 * go of on every path that could otherwise leave LVGL drawing from freed
 * memory.
 *
 * Real: LVGL and its software renderer, the screensaver, the scrolling labels,
 * the themes, the settings card helpers, the translations and the config file.
 * Substituted: the player that lends the artwork, the cover loader -- driven by
 * hand, so the test decides when a decode lands and what it holds -- and the
 * device services. */
#include <assert.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/device/screensaverpics.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"

/* ---- fonts: every face is LVGL's built-in one ---------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_72;

static void init_fonts(void) {
	lv_font_t *fonts[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		*fonts[i] = lv_font_montserrat_14;
	}
}

void fonts_register_change(void (*cb)(void)) { (void)cb; }
int fonts_text_size(void) { return FONTS_TEXT_NORMAL; }

/* ---- device services ------------------------------------------------------ */

static bool screen_on; // what power_screen_is_on() answers: false going dark, true waking

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}

void bootlogo_set_force(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool clock_is_set(void) { return true; }
void clock_format_hm(char *out, size_t out_size, int hour, int minute) {
	(void)hour;
	(void)minute;
	snprintf(out, out_size, "12:34"); // a fixed time, so snapshots compare
}
bool power_screen_is_on(void) { return screen_on; }
void power_notify_activity(void) {}
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }
void screensaverpics_set_root(const char *card_root) { (void)card_root; }
// The largest picture the screensaver last asked the folder for, and how many
// times it has asked. The folder itself is tested on its own
// (tests/test_screensaverpics.c); what is checked here is the limit it is
// handed.
static int folder_max_w, folder_max_h, folder_asks;
bool screensaverpics_any(int max_w, int max_h) {
	folder_max_w = max_w;
	folder_max_h = max_h;
	folder_asks++;
	return false;
}
bool screensaverpics_pick(const char *current, int max_w, int max_h, char *out, size_t out_size) {
	(void)current;
	(void)out;
	(void)out_size;
	folder_max_w = max_w;
	folder_max_h = max_h;
	folder_asks++;
	return false;
}
const char *metadata_shown_artist(const song_metadata_t *m) { return m->artist; }

/* ---- pictures, and the bookkeeping that catches a leak or a double free --- */

static int live_buffers; // pixel buffers made here and not yet freed

// A one-pixel checkerboard over two slow gradients: any resampling at all
// blends the checks, and any shift of even a pixel moves the gradients. `seed`
// tells two pictures of the same size apart.
static void make_picture(cover_image_t *img, int w, int h, int seed) {
	memset(img, 0, sizeof(*img));
	img->pixels = malloc((size_t)w * (size_t)h * 2);
	assert(img->pixels);
	live_buffers++;
	uint16_t *px = (uint16_t *)img->pixels;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint16_t r = (uint16_t)((x * 31) / (w > 1 ? w - 1 : 1));
			uint16_t g = (uint16_t)((((y * 63) / (h > 1 ? h - 1 : 1)) ^ (seed * 21)) & 63);
			uint16_t b = ((x + y) & 1) ? 31 : (uint16_t)(seed & 7);
			px[y * w + x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
	img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	img->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	img->dsc.header.w = (uint32_t)w;
	img->dsc.header.h = (uint32_t)h;
	img->dsc.header.stride = (uint32_t)w * 2;
	img->dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
	img->dsc.data = img->pixels;
}

void cover_free(cover_image_t *img) {
	if (!img) {
		return;
	}
	if (img->pixels) {
		live_buffers--;
		assert(live_buffers >= 0);
	}
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

/* ---- the player: what it lends ------------------------------------------- */

static cover_image_t player_cover, player_backdrop;
static char player_path[256];
static device_state_t state;

const lv_image_dsc_t *player_cover_image(void) { return player_cover.pixels ? &player_cover.dsc : NULL; }
const lv_image_dsc_t *player_backdrop_image(void) { return player_backdrop.pixels ? &player_backdrop.dsc : NULL; }
const char *player_cover_path(void) { return player_cover.pixels && player_path[0] ? player_path : NULL; }
void device_state_get(device_state_t *out) { *out = state; }

// A track change, the way the player makes one: the release hook first, then
// its own buffers freed, then -- for a track with artwork -- the new picture
// once its decode lands, which the screensaver notices on its next tick.
static void player_change_track(const char *path, const char *title) {
	screensaver_release_artwork();
	cover_free(&player_cover);
	cover_free(&player_backdrop);
	snprintf(player_path, sizeof(player_path), "%s", path);
	snprintf(state.current_file, sizeof(state.current_file), "%s", path);
	snprintf(state.metadata.title, sizeof(state.metadata.title), "%s", title);
}

static void player_lands_art(int sleeve_w, int sleeve_h, int seed) {
	make_picture(&player_cover, sleeve_w, sleeve_h, seed);
	// The blur, in the shape the player makes it: the whole V1 screen, the
	// deck's strip on the regular panels.
	make_picture(&player_backdrop, bp_screen_w(), bp_is_tempotec_v1() ? bp_screen_h() : 240, seed + 50);
}

/* ---- the cover loader, by hand -------------------------------------------- */

static struct {
	bool pending; // asked for, not yet collected or abandoned
	bool landed;  // the decode has finished; the next take hands it over
	bool found;
	cover_image_t result;
	char path[256];
	int w, h;
	int requests, releases;
} frame_job;

void coverloader_request_frame(const char *path, int w, int h) {
	cover_free(&frame_job.result); // newest wins: whatever was in flight is gone
	frame_job.pending = true;
	frame_job.landed = false;
	snprintf(frame_job.path, sizeof(frame_job.path), "%s", path);
	frame_job.w = w;
	frame_job.h = h;
	frame_job.requests++;
}

bool coverloader_take_frame(cover_image_t *image, bool *finished) {
	memset(image, 0, sizeof(*image));
	*finished = false;
	if (!frame_job.pending || !frame_job.landed) {
		return false;
	}
	*finished = true;
	frame_job.pending = false;
	*image = frame_job.result;
	memset(&frame_job.result, 0, sizeof(frame_job.result));
	return frame_job.found;
}

void coverloader_release_frame(void) {
	frame_job.pending = false;
	frame_job.landed = false;
	cover_free(&frame_job.result);
	frame_job.releases++;
}

// The worker finishing the decode it was asked for: `w` x `h` pixels, or
// nothing at all.
static void frame_decode_lands(int w, int h, int seed) {
	assert(frame_job.pending && !frame_job.landed);
	frame_job.landed = true;
	frame_job.found = w > 0;
	if (frame_job.found) {
		make_picture(&frame_job.result, w, h, seed);
	}
}

// The folder pictures' job: never asked to finish here.
void coverloader_request_screensaver(const char *path, int w, int h, int strip_w, int strip_h) {
	(void)path;
	(void)w;
	(void)h;
	(void)strip_w;
	(void)strip_h;
}
bool coverloader_take_screensaver(cover_image_t *image, cover_image_t *strip, bool *finished) {
	memset(image, 0, sizeof(*image));
	memset(strip, 0, sizeof(*strip));
	*finished = false;
	return false;
}
void coverloader_release_screensaver(void) {}

/* ---- the screensaver's tree, as screensaver_init() builds it -------------- */

static int screen_w, screen_h;
static int strip_h;	   // DEFAULT_STRIP_H
static int frame_side; // what the margins leave: 160 on the V1
static int radius;	   // the player's sleeve radius

static lv_obj_t *saver_obj(void) {
	assert(lv_obj_get_child_count(lv_layer_top()) >= 1);
	return lv_obj_get_child(lv_layer_top(), 0);
}
static lv_obj_t *cover_box(void) { return lv_obj_get_child(saver_obj(), 0); }
static lv_obj_t *placeholder(void) { return lv_obj_get_child(cover_box(), 0); }
static lv_obj_t *frame(void) { return lv_obj_get_child(cover_box(), 1); }
static lv_obj_t *picture(void) { return lv_obj_get_child(frame(), 0); }
static lv_obj_t *info_strip(void) { return lv_obj_get_child(saver_obj(), 1); }

// The picture on screen, as the screensaver holds it: the descriptor LVGL was
// given is the first member of the cover_image_t that owns the pixels.
static const cover_image_t *shown_picture(void) {
	const lv_image_dsc_t *dsc = lv_image_get_src(picture());
	assert(dsc);
	return (const cover_image_t *)((const uint8_t *)dsc - offsetof(cover_image_t, dsc));
}

static void pump(uint32_t ms) {
	for (uint32_t t = 0; t < ms; t += 10) {
		lv_tick_inc(10);
		lv_timer_handler();
	}
}

// One tick of the screensaver's own once-a-second timer.
static void tick(void) { pump(1010); }

static void hide_now(void) {
	screensaver_hide();
	for (int i = 0; i < 100 && screensaver_is_visible(); i++) {
		pump(10);
	}
	assert(!screensaver_is_visible());
}

static void go_dark(void) {
	screen_on = false;
	assert(screensaver_prepare());
}

static void wake(void) {
	screen_on = true;
	assert(screensaver_prepare());
}

/* Optional real-renderer snapshots for review, kept out of Git. */
static void dump_snapshot(const char *name) {
	const char *prefix = getenv("MORPTHIS_SCREENSAVER_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(saver_obj());
	lv_draw_buf_t *shot = lv_snapshot_take(saver_obj(), LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%d-%s.ppm", prefix, screen_w, name);
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

/* ---- what Framed promises ------------------------------------------------- */

// Every pixel of the frame outside its rounded corners is the picture's own,
// untouched: drawn one to one, not shifted, not blended with a neighbour. Any
// resampling at all fails this -- the picture is a one-pixel checkerboard --
// and it is what "never enlarged, never pixelated" looks like on the glass.
static void check_one_to_one(const cover_image_t *pic) {
	lv_obj_update_layout(saver_obj());
	assert(lv_image_get_src(picture()) == &pic->dsc);
	assert(lv_image_get_scale(picture()) == LV_SCALE_NONE);
	lv_area_t f;
	lv_obj_get_coords(frame(), &f);
	int w = (int)lv_area_get_width(&f), h = (int)lv_area_get_height(&f);
	assert(w == (int)pic->dsc.header.w && h == (int)pic->dsc.header.h);

	lv_draw_buf_t *shot = lv_snapshot_take(saver_obj(), LV_COLOR_FORMAT_RGB565);
	assert(shot && (int)shot->header.w == screen_w && (int)shot->header.h == screen_h);
	int checked = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if ((x < radius || x >= w - radius) && (y < radius || y >= h - radius)) {
				continue; // a rounded corner: the page shows there by design
			}
			uint16_t got, want;
			memcpy(&got, shot->data + (size_t)(f.y1 + y) * shot->header.stride + (size_t)(f.x1 + x) * 2, 2);
			memcpy(&want, pic->pixels + ((size_t)y * (size_t)w + (size_t)x) * 2, 2);
			assert(got == want);
			checked++;
		}
	}
	assert(checked == w * h - 4 * radius * radius); // all of it but the corner squares
	lv_draw_buf_destroy(shot);
}

// The frame: `w` x `h`, centred in the area above the strip with at least the
// margins round it, the corners rounded like the player's sleeve; the words
// below it on the page itself, no strip of their own.
static void check_frame(int w, int h) {
	lv_obj_update_layout(saver_obj());
	assert(!lv_obj_is_hidden(picture()));
	assert(lv_obj_is_hidden(placeholder()));
	assert(w <= frame_side && h <= frame_side);

	lv_area_t box, f, s;
	lv_obj_get_coords(cover_box(), &box);
	lv_obj_get_coords(frame(), &f);
	lv_obj_get_coords(info_strip(), &s);
	assert(lv_area_get_width(&f) == w && lv_area_get_height(&f) == h);
	assert(box.y1 == 0 && lv_area_get_height(&box) == screen_h - strip_h);
	assert(s.y1 == screen_h - strip_h && lv_area_get_height(&s) == strip_h);
	assert(abs((f.x1 + f.x2) - (screen_w - 1)) <= 1);
	assert(abs((f.y1 + f.y2) - (box.y1 + box.y2)) <= 1);
	int side_margin = bp_pick(16, 40), vert_margin = bp_pick(14, 32);
	assert(f.x1 >= side_margin && f.x2 <= screen_w - 1 - side_margin);
	assert(f.y1 >= vert_margin && f.y2 <= s.y1 - 1 - vert_margin);
	assert(lv_obj_get_style_radius(frame(), 0) == radius);
	assert(lv_obj_get_style_clip_corner(frame(), 0));
	assert(lv_obj_get_style_bg_opa(info_strip(), 0) == LV_OPA_TRANSP);
	assert(lv_obj_get_style_bg_image_src(info_strip(), 0) == NULL);
}

// What the page round the frame is: the player's blurred copy when it made
// one the size of the screen, black otherwise.
static void check_page(void) {
	const lv_image_dsc_t *backdrop = player_backdrop_image();
	bool blur = backdrop && (int)backdrop->header.w == screen_w && (int)backdrop->header.h == screen_h;
	assert(lv_obj_get_style_bg_image_src(saver_obj(), 0) == (blur ? (const void *)backdrop : NULL));
	assert(lv_obj_get_style_bg_opa(cover_box(), 0) == (blur ? LV_OPA_TRANSP : LV_OPA_COVER));
	lv_color_t page = lv_color_black();
	assert(lv_color_eq(lv_obj_get_style_bg_color(saver_obj(), 0), page));
	assert(lv_color_eq(lv_obj_get_style_bg_color(cover_box(), 0), page));
}

// Fill as it always was: the artwork covers its whole area, enlarged to do it
// wherever it is smaller than the area -- on the V1 it always is, which is the
// reason Framed exists -- with the frame square and the size of the area, and
// nothing borrowed behind the page.
static void check_fill(const cover_image_t *sleeve) {
	lv_obj_update_layout(saver_obj());
	assert(lv_image_get_src(picture()) == &sleeve->dsc);
	int32_t scale = lv_image_get_scale(picture());
	int area_w = (int)lv_obj_get_width(cover_box()), area_h = (int)lv_obj_get_height(cover_box());
	int drawn_w = (int)(((int32_t)sleeve->dsc.header.w * scale) / LV_SCALE_NONE);
	int drawn_h = (int)(((int32_t)sleeve->dsc.header.h * scale) / LV_SCALE_NONE);
	// Edge to edge, give or take the pixel the zoom's rounding loses, and no
	// larger than covering takes: one side fits.
	assert(drawn_w >= area_w - 1 && drawn_h >= area_h - 1);
	assert(drawn_w <= area_w + 1 || drawn_h <= area_h + 1);
	if (bp_is_tempotec_v1()) {
		assert(scale > LV_SCALE_NONE); // the V1's sleeve is smaller than any area it fills
	}
	assert(lv_obj_get_width(frame()) == lv_obj_get_width(cover_box()));
	assert(lv_obj_get_height(frame()) == lv_obj_get_height(cover_box()));
	assert(lv_obj_get_style_radius(frame(), 0) == 0);
	assert(lv_obj_get_style_bg_image_src(saver_obj(), 0) == NULL);
	assert(lv_obj_get_style_bg_opa(cover_box(), 0) == LV_OPA_COVER);
}

static void check_nothing_held(void) {
	assert(!frame_job.pending);
	assert(live_buffers == (player_cover.pixels ? 1 : 0) + (player_backdrop.pixels ? 1 : 0));
}

/* ---- the settings control ------------------------------------------------- */

static void noop_cb(lv_event_t *e) { (void)e; }

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

// The card on the Screen page: the switch, Album and Pictures, and under them
// a captioned second row for how the album is drawn. Built with the same
// helpers settings.c uses; the wiring that hides the group is a single line
// there (screensaver_refresh).
static void check_settings_card(const char *language_dir) {
	lv_obj_t *previous = lv_screen_active();
	lv_obj_t *page = lv_obj_create(NULL);
	lv_obj_set_size(page, screen_w, screen_h);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_screen_load(page);

	lv_obj_t *toggle = NULL, *pills = NULL, *style_pills = NULL;
	lv_obj_t *card = settingsrow_toggle_pills(page, "screensaver", noop_cb, &toggle, &pills);
	lv_obj_t *album = settingsrow_pill(pills, "screensaver_source_album", 0, noop_cb);
	settingsrow_pill(pills, "screensaver_source_images", 1, noop_cb);
	lv_obj_t *group = settingsrow_pills_group(card, "screensaver_album_art", &style_pills);
	lv_obj_t *fill = settingsrow_pill(style_pills, "screensaver_style_fill", SCREENSAVER_STYLE_FILL, noop_cb);
	lv_obj_t *framed = settingsrow_pill(style_pills, "screensaver_style_framed", SCREENSAVER_STYLE_FRAMED, noop_cb);
	settingsrow_pill_active(fill, false);
	settingsrow_pill_active(framed, true);
	lv_obj_update_layout(page);

	assert(lv_obj_get_parent(group) == card && lv_obj_get_parent(style_pills) == group);
	lv_obj_t *caption = lv_obj_get_child(group, 0);
	assert(lv_obj_check_type(caption, &lv_label_class));
	assert(strcmp(lv_label_get_text(caption), "Album art") == 0);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(fill, 0)), "Fill") == 0);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(framed, 0)), "Framed") == 0);

	// Under the first row, inside the card, the two pills side by side, laid
	// out exactly as the first row's are.
	lv_area_t c, g, a, f1, f2;
	lv_obj_get_coords(card, &c);
	lv_obj_get_coords(group, &g);
	lv_obj_get_coords(album, &a);
	lv_obj_get_coords(fill, &f1);
	lv_obj_get_coords(framed, &f2);
	assert(g.y1 > a.y2);
	assert(g.x1 >= c.x1 && g.x2 <= c.x2 && g.y2 <= c.y2);
	assert(f1.y1 == f2.y1 && f2.x1 > f1.x2);
	assert(f2.x2 <= c.x2);
	assert(lv_obj_get_height(fill) == lv_obj_get_height(album));
	assert(lv_obj_get_style_pad_column(style_pills, 0) == lv_obj_get_style_pad_column(pills, 0));
	assert(lv_obj_get_style_pad_row(style_pills, 0) == lv_obj_get_style_pad_row(pills, 0));

	// Hidden, it gives its room back.
	int32_t tall = lv_obj_get_height(card);
	lv_obj_set_hidden(group, true);
	lv_obj_update_layout(page);
	assert(lv_obj_get_height(card) < tall);
	lv_obj_set_hidden(group, false);

	// Every language has the three texts -- a missing one would show its
	// bare tag -- and what is on screen follows a switch.
	const char *keys[] = {"screensaver_album_art", "screensaver_style_fill", "screensaver_style_framed"};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			const char *text = tr(keys[i]);
			assert(text && text[0] && strcmp(text, keys[i]) != 0);
		}
		assert(strcmp(lv_label_get_text(caption), tr("screensaver_album_art")) == 0);
		assert(strcmp(lv_label_get_text(lv_obj_get_child(fill, 0)), tr("screensaver_style_fill")) == 0);
		assert(strcmp(lv_label_get_text(lv_obj_get_child(framed, 0)), tr("screensaver_style_framed")) == 0);
		languages++;
	}
	closedir(dir);
	assert(languages == 11);
	assert(lang_set("English"));
	assert(strcmp(lv_label_get_text(caption), "Album art") == 0);
	lv_screen_load(previous);
	lv_obj_delete(page);
}

/* ---- the Screensaver folder's size limit ---------------------------------- */

static int note_max_w, note_max_h; // the size the empty-folder note promises

// The size the note shown for an empty folder promises, in WxH form, or false.
static bool note_size(const char *note, int *w, int *h) {
	for (const char *p = note; *p; p++) {
		if (*p >= '0' && *p <= '9' && (p == note || p[-1] < '0' || p[-1] > '9') && sscanf(p, "%dx%d", w, h) == 2) {
			return true;
		}
	}
	return false;
}

// The limit the screensaver hands the folder is the one its own note states, in
// every language. Until this was checked, the V1 told people pictures could be
// up to 480x800, from the R1 texts it ships, while it allowed nothing larger
// than its 240x320 panel and so turned away every picture made to the note.
static void check_picture_limit(const char *language_dir) {
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		int w = 0, h = 0;
		assert(note_size(tr("screensaver_empty_note"), &w, &h));
		if (languages == 0) {
			note_max_w = w;
			note_max_h = h;
		}
		assert(w == note_max_w && h == note_max_h); // one promise, in every language
		languages++;
	}
	closedir(dir);
	assert(languages == 11);
	assert(lang_set("English"));

	int asks = folder_asks;
	assert(!screensaver_has_images());
	assert(folder_asks == asks + 1);
	assert(folder_max_w == note_max_w && folder_max_h == note_max_h);
	if (bp_is_tempotec_v1()) {
		// Larger than the panel, so the loader is asked for the panel's size
		// and brings the picture down to it.
		assert(note_max_w > screen_w && note_max_h > screen_h);
	}
}

int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	bool saved_framed = argc == 3 && strcmp(argv[2], "saved") == 0;

	char dir[] = "/tmp/morpthis-screensaver-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config);
	config_set_bool("screen", "screensaver", true);
	if (saved_framed) {
		config_set_int("screen", "screensaver_style", SCREENSAVER_STYLE_FRAMED);
	}
	assert(config_save());
	assert(config_init(config));
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	init_fonts();
	lv_init();

	screen_w = bp_screen_w();
	screen_h = bp_screen_h();
	strip_h = bp_pick(132, 250);
	radius = bp_pick(14, 18);
	int side_w = screen_w - 2 * bp_pick(16, 40);
	int side_h = screen_h - strip_h - 2 * bp_pick(14, 32);
	frame_side = (side_w < side_h ? side_w : side_h) & ~1;
	if (bp_is_tempotec_v1()) {
		// The V1: about the size the player draws its own sleeve, with 40 px
		// either side and 14 above and below.
		assert(screen_w == 240 && screen_h == 320 && frame_side == 160);
	} else {
		assert(frame_side == 400);
	}

	gui_config_t cfg = {screen_w, screen_h, bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_t *display = lv_display_create(cfg.screen_width, cfg.screen_height);
	lv_display_delete_refr_timer(display); // nothing to flush to; timers below are the screensaver's own
	theme_init();
	theme_set_kind(THEME_KIND_DARK);

	// The player's sleeve, at the size the player decodes it: 156 px on the
	// V1, the panel's whole 480 px width on the regular panels -- larger than
	// their frame, so it has to come down to fit.
	int sleeve = bp_pick(156, 480);
	player_change_track("/mnt/sd/Music/OK Computer/02 Paranoid Android.flac", "Paranoid Android");
	snprintf(state.metadata.artist, sizeof(state.metadata.artist), "Radiohead");
	player_lands_art(sleeve, sleeve, 1);

	screensaver_init(&cfg);
	assert(screensaver_get_enabled());
	assert(screensaver_source() == SCREENSAVER_SOURCE_ALBUM);
	check_picture_limit(argv[1]);
	assert(screensaver_style() == (saved_framed ? SCREENSAVER_STYLE_FRAMED : SCREENSAVER_STYLE_FILL));
	assert(lv_obj_check_type(placeholder(), &lv_image_class));
	assert(lv_obj_check_type(picture(), &lv_image_class));

	/* Fill, unchanged: the default, and what an existing setup keeps. */
	screensaver_set_style(SCREENSAVER_STYLE_FILL);
	assert(config_init(config) && config_get_int("screen", "screensaver_style", -1) == SCREENSAVER_STYLE_FILL);
	go_dark();
	wake();
	check_fill(&player_cover);
	assert(frame_job.requests == 0); // Fill never asks for a decode
	dump_snapshot("fill-dark");
	hide_now();
	check_nothing_held();

	/* Framed. Chosen and saved; nothing decoded until the screen goes dark. */
	screensaver_set_style(SCREENSAVER_STYLE_FRAMED);
	assert(screensaver_style() == SCREENSAVER_STYLE_FRAMED);
	assert(config_init(config) && config_get_int("screen", "screensaver_style", -1) == SCREENSAVER_STYLE_FRAMED);
	assert(frame_job.requests == 0);

	// Going dark asks for the copy at exactly the frame's size, of the file
	// the player read its picture from...
	go_dark();
	assert(frame_job.pending && frame_job.requests == 1);
	assert(strcmp(frame_job.path, player_path) == 0);
	assert(frame_job.w == frame_side && frame_job.h == frame_side);
	// ...and until it lands the player's sleeve stands in, never enlarged:
	// one to one where it fits, brought down where it does not.
	if (sleeve <= frame_side) {
		check_frame(sleeve, sleeve);
		check_one_to_one(&player_cover);
	} else {
		int32_t scale = lv_image_get_scale(picture());
		assert(scale < LV_SCALE_NONE);
		check_frame(sleeve * scale / LV_SCALE_NONE, sleeve * scale / LV_SCALE_NONE);
	}
	check_page();

	// The decode lands while the panel is dark; the wake opens on it.
	frame_decode_lands(frame_side, frame_side, 2);
	wake();
	assert(!frame_job.pending && frame_job.requests == 1);
	const cover_image_t *held = shown_picture(); // the screensaver's own copy
	assert(held != &player_cover && held->pixels);
	check_frame(frame_side, frame_side);
	check_one_to_one(held);
	check_page();
	if (bp_is_tempotec_v1()) {
		// The numbers the panel actually shows: 40 px either side, 14 above
		// and below, the strip's 132 px under it.
		lv_area_t f;
		lv_obj_get_coords(frame(), &f);
		assert(f.x1 == 40 && f.y1 == 14 && f.x2 == 199 && f.y2 == 173);
	}

	// Every theme: same frame, same pixels, the page each one asks for -- and
	// no decode asked for again just because the colours changed.
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		check_frame(frame_side, frame_side);
		check_one_to_one(held);
		check_page();
		char name[32];
		snprintf(name, sizeof(name), "framed-theme%d", kind);
		dump_snapshot(name);
	}
	theme_set_kind(THEME_KIND_DARK);
	assert(frame_job.requests == 1);

	/* A track change while it is up: let go of everything, then the next
	 * picture -- stand-in first, its own copy on a later tick. */
	int live_before = live_buffers;
	player_change_track("/mnt/sd/Music/Kid A/01 Everything In Its Right Place.flac", "Everything In Its Right Place");
	assert(live_buffers == live_before - 3); // the framed copy and the player's two
	assert(lv_image_get_src(picture()) == NULL && lv_obj_is_hidden(picture()));
	assert(!lv_obj_is_hidden(placeholder()));
	assert(lv_obj_get_style_bg_image_src(saver_obj(), 0) == NULL); // the blur was the player's
	// No artwork yet: nothing to ask for.
	tick();
	assert(frame_job.requests == 1 && !lv_obj_is_hidden(placeholder()));
	player_lands_art(sleeve, sleeve, 3);
	tick();
	assert(frame_job.pending && frame_job.requests == 2 && strcmp(frame_job.path, player_path) == 0);
	assert(lv_image_get_src(picture()) == &player_cover.dsc);
	frame_decode_lands(frame_side, frame_side, 4);
	tick(); // collected and put straight up: the same picture, only sharper
	held = shown_picture();
	assert(held != &player_cover);
	check_frame(frame_side, frame_side);
	check_one_to_one(held);

	/* A track change with the decode still in flight abandons it. */
	player_change_track("/mnt/sd/Music/Amnesiac/01 Packt Like Sardines.flac", "Packt Like Sardines");
	player_lands_art(sleeve, sleeve, 5);
	tick();
	assert(frame_job.pending && frame_job.requests == 3);
	int releases = frame_job.releases;
	player_change_track("/mnt/sd/Music/Amnesiac/02 Pyramid Song.flac", "Pyramid Song");
	assert(!frame_job.pending && frame_job.releases == releases + 1);

	/* A file whose picture this decode cannot read: the stand-in stays, and
	 * it is not asked again tick after tick -- until the picture changes. */
	player_lands_art(sleeve, sleeve, 6);
	tick();
	assert(frame_job.requests == 4);
	frame_decode_lands(0, 0, 0);
	tick();
	tick();
	tick();
	assert(frame_job.requests == 4 && !frame_job.pending);
	assert(lv_image_get_src(picture()) == &player_cover.dsc);
	assert(lv_image_get_scale(picture()) <= LV_SCALE_NONE);
	player_change_track("/mnt/sd/Music/Amnesiac/03 Pulk Pull Revolving Doors.flac", "Pulk/Pull Revolving Doors");
	player_lands_art(sleeve, sleeve, 7);
	tick();
	assert(frame_job.requests == 5 && frame_job.pending);

	/* A file that only carries a small picture: shown at its own size. */
	frame_decode_lands(100, 100, 8);
	tick();
	held = shown_picture();
	check_frame(100, 100);
	check_one_to_one(held);
	dump_snapshot("framed-small");

	/* Immersive lends a picture cropped to the screen's shape: framed, it
	 * would be a slice of the record, so the note stands in until the copy
	 * of the whole picture lands. */
	player_change_track("/mnt/sd/Music/Amnesiac/04 You And Whose Army.flac", "You And Whose Army?");
	player_lands_art(screen_w, screen_h, 9);
	tick();
	assert(frame_job.pending && frame_job.requests == 6);
	assert(lv_obj_is_hidden(picture()) && !lv_obj_is_hidden(placeholder()));
	frame_decode_lands(frame_side, frame_side, 10);
	tick();
	held = shown_picture();
	check_frame(frame_side, frame_side);
	check_one_to_one(held);

	/* Back to Fill while it is up, with a decode in flight: the copy and the
	 * request go, and the old arrangement returns in every theme. */
	player_change_track("/mnt/sd/Music/Amnesiac/05 I Might Be Wrong.flac", "I Might Be Wrong");
	player_lands_art(sleeve, sleeve, 11);
	tick();
	assert(frame_job.pending && frame_job.requests == 7);
	releases = frame_job.releases;
	screensaver_set_style(SCREENSAVER_STYLE_FILL);
	assert(!frame_job.pending && frame_job.releases == releases + 1);
	check_fill(&player_cover);
	check_nothing_held();
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		check_fill(&player_cover);
		char name[32];
		snprintf(name, sizeof(name), "fill-theme%d", kind);
		dump_snapshot(name);
	}
	theme_set_kind(THEME_KIND_DARK);
	tick();
	assert(frame_job.requests == 7); // Fill asks for nothing

	/* Framed again while it is up: the redraw asks at once. Then dismissed
	 * with that request in flight: nothing is kept, nothing is left pointing
	 * at anything. */
	screensaver_set_style(SCREENSAVER_STYLE_FRAMED);
	assert(frame_job.pending && frame_job.requests == 8);
	check_frame(sleeve <= frame_side ? sleeve : lv_obj_get_width(frame()),
				sleeve <= frame_side ? sleeve : lv_obj_get_height(frame()));
	releases = frame_job.releases;
	hide_now();
	assert(frame_job.releases == releases + 1);
	check_nothing_held();
	assert(lv_image_get_src(picture()) == NULL);
	assert(lv_obj_get_style_bg_image_src(saver_obj(), 0) == NULL);

	/* Pictures from the folder are drawn the one way, whatever the style. */
	go_dark();
	assert(frame_job.pending);
	int asks = folder_asks;
	screensaver_set_source(SCREENSAVER_SOURCE_IMAGES);
	assert(folder_asks == asks + 1); // the first pick, asked for at once
	assert(folder_max_w == note_max_w && folder_max_h == note_max_h);
	assert(!frame_job.pending);
	check_nothing_held();
	wake();
	lv_obj_update_layout(saver_obj());
	assert(lv_obj_get_width(frame()) == screen_w && lv_obj_get_height(frame()) == screen_h);
	assert(lv_obj_get_style_radius(frame(), 0) == 0);
	assert(lv_obj_get_style_bg_image_src(saver_obj(), 0) == NULL);
	int requests = frame_job.requests;
	tick();
	assert(frame_job.requests == requests);
	hide_now();
	screensaver_set_source(SCREENSAVER_SOURCE_ALBUM);

	/* Switched off with the screensaver up: everything goes with it. */
	go_dark();
	assert(frame_job.pending);
	screensaver_set_enabled(false);
	for (int i = 0; i < 100 && screensaver_is_visible(); i++) {
		pump(10);
	}
	assert(!screensaver_is_visible());
	check_nothing_held();

	check_settings_card(argv[1]);

	cover_free(&player_cover);
	cover_free(&player_backdrop);
	assert(live_buffers == 0);
	lv_deinit();
	unlink(config);
	rmdir(dir);
	printf("PASS screensaver framed artwork (%dx%d, %s)\n", screen_w, screen_h,
		   saved_framed ? "saved setting" : "default setting");
	return 0;
}
