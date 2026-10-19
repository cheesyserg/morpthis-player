#include "screensaver.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/power.h"
#include "src/system/device/screensaverpics.h"

// Movement that turns a press into a drag, and how long the slide that follows
// the release lasts. The dismiss threshold itself is a quarter of the screen,
// applied in drag_cb; short of it the panel slides back down, like the control
// centre.
#define DRAG_ENGAGE_PX 8
#define DISMISS_ANIM_MS 180

static lv_obj_t *saver;		 // the whole thing, full screen on the top layer
static lv_obj_t *cover_img;	 // the artwork: above the strip, or the whole panel under a field
static lv_obj_t *cover_placeholder; // the note, for a track that has none
static lv_obj_t *info_strip; // metadata over blur, or a translucent panel in the V1 palettes
static lv_obj_t *cover_box;	 // the artwork's area: everything above the strip, or the screen
static lv_obj_t *frame_box;	 // what cover_img sits in: all of cover_box, or the Framed sleeve
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *clock_label;
static lv_obj_t *chevron_icon;
static lv_timer_t *clock_timer;

static bool enabled;
static int screen_w, screen_h;
static int strip_h;

// What the strip falls back to when there is no artwork to size it from, and
// the height it always has over a picture from the folder -- there the block
// is built to order rather than borrowed, so there is nothing to measure. The
// two V1 palettes use this same stable height for the metadata field they lay
// over the sleeve rather than under it.
#define DEFAULT_STRIP_H bp_pick(132, 250)

// How much of the sleeve still shows through that field, per palette. Amber's
// cream and amber type is bright enough to read through half; Tokyo Night's
// violet, cyan and blue are dimmer inks, so its film is a step heavier.
#define AMBER_STRIP_OPA LV_OPA_50
#define TOKYO_STRIP_OPA LV_OPA_60

// The Framed style's sleeve. It keeps at least this much of the panel free on
// either side, and above and below itself inside the area over the strip; its
// side is the largest square both allow. That is worked out once, from the
// panel and the strip's default height, so it is the same at every wake and
// one decode serves every frame drawn from it: 160 px on the V1, with 40 px
// either side and 14 above and below -- the size the player draws its own
// sleeve at, near enough -- and 400 px on the 480-wide panels.
#define FRAME_MARGIN_SIDE bp_pick(16, 40)
#define FRAME_MARGIN_VERT bp_pick(14, 32)
// The player's own sleeve radius: framed, the artwork is recognisably the
// sleeve the player shows rather than a different picture.
#define FRAME_RADIUS bp_pick(14, 18)

// ---------------------------------------------------------------------------
// Pictures from the card
//
// The other thing the screensaver can show: one of the JPEGs in the card's
// Screensaver folder, a different one each time the screen lights up. Which
// files qualify and which one comes up is screensaverpics.h; what is left here
// is the drawing and the pixels it owns.
//
// Nothing is decoded on this thread. The picture for the next wake is asked of
// the cover loader as the panel goes dark and is waiting, decoded, by the time
// the screen comes back -- so a wake costs a pointer swap rather than a card
// read, a JPEG and a blur with the interface thread standing still and the
// audio decoder sharing the one core with it.
// ---------------------------------------------------------------------------

static screensaver_source_t source;

// The largest picture the folder is read for. Elsewhere that is the panel
// itself, so a picture is drawn as it was made and never shrunk (see
// screensaverpics.h).
//
// The V1 is the exception. Its firmware carries the R1's resource tree, texts
// included, and the note shown when the folder holds nothing usable says
// pictures may be up to the R1's 480x800. With its own 240x320 panel as the
// limit, the V1 turned every such picture away, and the note sent people
// looking for a mistake that was not theirs. So the V1 takes what the note
// promises. The cover loader brings each picture down to the panel, cropped to
// fill it like an album cover, through the scaled decode album covers use. At
// that size a picture is about 1.1 MB decoded.
#define V1_PICTURE_MAX_W 480
#define V1_PICTURE_MAX_H 800
static int picture_max_w, picture_max_h;

// The picture on screen and, outside the V1 palettes, the blurred block over
// its lower part. Owned here, not by the player: freed here whenever a
// different one is chosen or the screensaver goes away.
static cover_image_t image_pic, image_strip;
static char image_file[512]; // what image_pic was built from

// The one being prepared for the next wake: `next_file` while it is out with
// the loader, and the pixels once they have been collected.
static cover_image_t next_pic, next_strip;
static char next_file[512];
static bool next_ready;	  // next_pic/next_strip hold a collected picture
static bool next_pending; // a request is out with the loader

// ---------------------------------------------------------------------------
// The Framed style
//
// The album source again, but drawn from a copy decoded here instead of from
// the player's: the player's is cropped and enlarged to its own sleeve (or to
// the whole screen, in Immersive), and covering the screensaver's area with it
// is exactly the enlarging that blurs it into blocks. This one is the whole
// picture fitted inside the frame and never enlarged, so it is drawn at one
// pixel to one.
//
// It is asked of the cover loader once the player has its picture, and kept
// until the player lets go of that picture (screensaver_release_artwork), the
// style or the source changes, or the screensaver goes away. As the panel goes
// dark the request rides on the redraw that leaves the next wake's frame in
// the framebuffer, so the decode happens over a screen nobody is looking at
// and the wake opens on the finished copy. Until it lands, the player's own
// sleeve stands in -- the same picture, at most one pixel to one.
// ---------------------------------------------------------------------------

static screensaver_style_t style;
static int frame_side; // the sleeve's side, worked out once (see FRAME_MARGIN_SIDE)

// Owned here, like a folder picture: freed here and nowhere else.
static cover_image_t frame_pic;
static bool frame_pending; // a request is out with the loader
// The loader came back with nothing for the picture now up: a source the
// player could read and this decode could not. Not asked again until the
// picture changes, or every tick would ask once more; the player's own sleeve
// stands in meanwhile.
static bool frame_failed;

// Resizes the strip and gives the picture whatever it should have.
//
// LVGL draws a background image at its own size, centred, and paints whatever
// is left over in the background colour, so a strip even a few pixels taller
// than the blurred block would show a black bar above it and another below.
// The height is therefore taken from the picture, leaving nothing to paint.
//
// In the regular themes an album cover stops where its blurred strip starts.
// A folder photograph already fills the screen, and so does the artwork under
// the V1 palettes' metadata field: that field is translucent, and it is only
// over the sleeve that it means anything.
//
// Cheap, and a no-op unless something has actually changed.
static int cover_box_h;

// The palettes added for the V1 draw the metadata over the sleeve itself:
// the artwork runs the whole panel and a translucent field of the palette's
// own colour is laid over its foot. Dark and Light keep the blurred strip,
// which is the artwork the player already made for that purpose.
static bool metadata_over_art(void) {
	return theme_is_monochrome() || theme_is_tokyo_night();
}

// The album, framed. In every theme the frame stands above the words rather
// than behind them, so the palettes' field and the blurred strip have nothing
// to lie over.
static bool framed_now(void) { return source == SCREENSAVER_SOURCE_ALBUM && style == SCREENSAVER_STYLE_FRAMED; }

static void set_strip_height(int height) {
	if (height <= 0 || height >= screen_h) {
		return;
	}
	if (height != strip_h) {
		strip_h = height;
		lv_obj_set_height(info_strip, strip_h);
		lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
	bool over_art = !framed_now() && (source == SCREENSAVER_SOURCE_IMAGES || metadata_over_art());
	int want = over_art ? screen_h : screen_h - strip_h;
	if (want != cover_box_h) {
		cover_box_h = want;
		lv_obj_set_height(cover_box, cover_box_h);
	}
}

bool screensaver_is_visible(void) { return saver && !lv_obj_is_hidden(saver); }
bool screensaver_get_enabled(void) { return enabled; }

screensaver_source_t screensaver_source(void) { return source; }

static void request_next(void);
static void drop_next(void);
static void drop_frame(void);
static void refresh_content(void);

void screensaver_set_source(screensaver_source_t chosen) {
	source = chosen == SCREENSAVER_SOURCE_IMAGES ? SCREENSAVER_SOURCE_IMAGES : SCREENSAVER_SOURCE_ALBUM;
	config_set_int("screen", "screensaver_source", (int)source);
	config_save();
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// The framed copy belongs to the album source; a folder picture never
		// needs it.
		drop_frame();
		// Asked for now rather than at the first screen-off, so the first wake
		// after the option is chosen already has a picture to show.
		request_next();
	} else {
		drop_next();
	}
}

screensaver_style_t screensaver_style(void) { return style; }

void screensaver_set_style(screensaver_style_t chosen) {
	style = chosen == SCREENSAVER_STYLE_FRAMED ? SCREENSAVER_STYLE_FRAMED : SCREENSAVER_STYLE_FILL;
	config_set_int("screen", "screensaver_style", (int)style);
	config_save();
	// Nothing is asked for here. The framed copy is asked for by the first
	// redraw that wants one: the one below when the screensaver is up, else
	// the one that leaves it in the framebuffer as the panel next goes dark --
	// and the player's sleeve stands in should a wake come before it lands.
	if (style != SCREENSAVER_STYLE_FRAMED) {
		drop_frame();
	}
	if (screensaver_is_visible()) {
		refresh_content();
	}
}

void screensaver_set_card_root(const char *root) { screensaverpics_set_root(root); }

bool screensaver_has_images(void) { return screensaverpics_any(picture_max_w, picture_max_h); }

// Picks the next picture and puts it out to decode. Called as the panel goes
// dark, which is the whole point: the card read and the JPEG happen on the
// loader thread at background priority over a screen nobody is looking at.
//
// A no-op while one is already in hand or in flight, so the repeated wake-hook
// calls cost nothing.
static void request_next(void) {
	if (source != SCREENSAVER_SOURCE_IMAGES || next_ready || next_pending) {
		return;
	}
	char chosen[sizeof(next_file)];
	if (!screensaverpics_pick(image_file, picture_max_w, picture_max_h, chosen, sizeof(chosen))) {
		// The folder gave nothing at all -- emptied, or the card taken out.
		// What is already up stays up: it is still a picture, and black is not.
		return;
	}
	snprintf(next_file, sizeof(next_file), "%s", chosen);
	next_pending = true;
	coverloader_request_screensaver(next_file, screen_w, screen_h, screen_w, DEFAULT_STRIP_H);
}

// Takes delivery of a finished decode, if there is one. Cheap and non-blocking:
// called at every wake and on the tick while the screensaver is up.
static void collect_next(void) {
	if (!next_pending) {
		return;
	}
	bool finished = false;
	cover_image_t pic = {0}, strip = {0};
	bool found = coverloader_take_screensaver(&pic, &strip, &finished);
	if (!finished) {
		return;
	}
	next_pending = false;
	if (!found) {
		// It passed the header check and still would not decode. Forgotten, so
		// the next request picks another rather than retrying this one.
		next_file[0] = '\0';
		return;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_pic = pic;
	next_strip = strip;
	next_ready = true;
}

static void drop_next(void) {
	if (next_pending) {
		coverloader_release_screensaver();
		next_pending = false;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_ready = false;
	next_file[0] = '\0';
}

static void drop_image(void) {
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_file[0] = '\0';
}

// Asks for the framed copy of the picture the player has up, when the Framed
// style wants one and none is in hand or on its way. Only once the player has
// that picture: a track with no artwork then costs nothing, and the path asked
// for is the one the player actually read it from -- a live stream's icon
// rather than the stream. A no-op otherwise, so it is safe at every redraw.
static void request_frame(void) {
	if (!enabled || !framed_now() || frame_pic.pixels || frame_pending || frame_failed || frame_side < 2) {
		return;
	}
	const char *path = player_cover_path();
	if (!path) {
		return;
	}
	frame_pending = true;
	coverloader_request_frame(path, frame_side, frame_side);
}

// Off the screen first, then freed: LVGL keeps drawing from the buffer it was
// given, and freeing it while it is still the widget's source is a
// use-after-free at the next redraw (see promote_next).
static void free_frame_pic(void) {
	if (cover_img && lv_image_get_src(cover_img) == &frame_pic.dsc) {
		lv_image_set_src(cover_img, NULL);
	}
	cover_free(&frame_pic);
}

// Takes delivery of a finished framed decode, if there is one. Cheap and
// non-blocking. True when a new picture came in, which the caller then puts up:
// unlike a folder picture this one may change while it is looked at, since it
// is the same artwork as the stand-in it replaces, only sharper.
static bool collect_frame(void) {
	if (!frame_pending) {
		return false;
	}
	bool finished = false;
	cover_image_t pic = {0};
	bool found = coverloader_take_frame(&pic, &finished);
	if (!finished) {
		return false;
	}
	frame_pending = false;
	if (!found) {
		frame_failed = true;
		return false;
	}
	// Empty by construction -- a request is only made with nothing in hand --
	// but whatever was there would be leaked, or drawn from after this.
	free_frame_pic();
	frame_pic = pic;
	return true;
}

static void drop_frame(void) {
	if (frame_pending) {
		coverloader_release_frame();
		frame_pending = false;
	}
	free_frame_pic();
	frame_failed = false;
}

void screensaver_set_enabled(bool on) {
	enabled = on;
	config_set_bool("screen", "screensaver", on);
	config_save();
	if (!on) {
		if (screensaver_is_visible()) {
			screensaver_hide(); // frees what is held on its way out
		} else {
			drop_next();
			drop_frame();
		}
	}
}

// ---------------------------------------------------------------------------
// what it shows
// ---------------------------------------------------------------------------

static void refresh_clock(void) {
	if (!clock_label) {
		return;
	}
	if (!clock_is_set()) {
		lv_label_set_text(clock_label, "--:--");
		return;
	}
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	char text[16];
	clock_format_hm(text, sizeof(text), local.tm_hour, local.tm_min);
	lv_label_set_text(clock_label, text);
}

// What refresh_content() last drew, so the tick below can tell an idle moment
// from a track that changed while the screensaver was up.
static char shown_file[512];
static const void *shown_cover;

// Runs while the screensaver is up. The clock is the obvious reason, but the
// important one is the track: the player frees its artwork at every track
// change and calls screensaver_release_artwork(), which blanks the picture, so
// this check is what puts the new one back.
static void clock_timer_cb(lv_timer_t *timer) {
	(void)timer;

	// Takes delivery of a decode that landed after the wake had already given
	// up on it -- a short dark spell, or a slow card. It is only stored, never
	// swapped in: the picture must not change while it is being looked at.
	collect_next();
	// The framed copy is the exception, and goes straight up: it is the
	// picture already on screen, only drawn from a sharper copy.
	bool framed_arrived = collect_frame();

	if (!screensaver_is_visible()) {
		refresh_clock();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	const void *cover = player_cover_image();

	const char *now = state.live ? state.metadata.title : state.current_file;
	if (framed_arrived || strcmp(now, shown_file) != 0 || cover != shown_cover) {
		refresh_content(); // refreshes the clock on its way through
		return;
	}

	refresh_clock();
}

// Puts the prepared picture on screen. Unlike the album source this owns its
// pixels, so the swap is the careful part: LVGL keeps drawing from the buffer
// it was given, and freeing the old one while it is still the widget's source
// is a use-after-free at the next redraw. Off the screen first, then free.
static void promote_next(void) {
	if (!next_ready) {
		return;
	}
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_pic = next_pic;
	image_strip = next_strip;
	snprintf(image_file, sizeof(image_file), "%s", next_file);

	memset(&next_pic, 0, sizeof(next_pic));
	memset(&next_strip, 0, sizeof(next_strip));
	next_ready = false;
	next_file[0] = '\0';
}

// The V1 palettes replace the artwork-derived blur beneath their metadata with
// a fixed translucent field: black for Amber and OLED, Tokyo Night's
// blue-black panel. Either keeps the palette's ink legible over every sleeve while still
// letting the cover show through behind it. The other themes keep the blurred
// copy, or plain black when there is no artwork to blur.
static void set_info_strip_backdrop(const lv_image_dsc_t *backdrop) {
	if (framed_now()) {
		// Framed keeps the words off the artwork altogether, so there is
		// nothing for a field or a strip to do: they are read on the page
		// itself, whichever page that is (see set_page_backdrop).
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
		return;
	}

	if (theme_is_monochrome()) {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, AMBER_STRIP_OPA, 0);
		return;
	}

	if (theme_is_tokyo_night()) {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, theme()->panel, 0);
		lv_obj_set_style_bg_opa(info_strip, TOKYO_STRIP_OPA, 0);
		return;
	}

	if (backdrop) {
		lv_obj_set_style_bg_image_src(info_strip, backdrop, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}
}

// What the whole page is drawn on behind a framed sleeve, in Dark and Light:
// the player's blurred copy of the artwork, when the player made it the size
// of the screen -- on the V1 it does for every arrangement but Immersive,
// whose blur is a band. NULL puts back the plain page colour. Borrowed exactly
// like the sleeve, so it is let go of in the same places: the release hook,
// and the screensaver going away.
static void set_page_backdrop(const lv_image_dsc_t *backdrop) {
	if (lv_obj_get_style_bg_image_src(saver, LV_PART_MAIN) != backdrop) {
		lv_obj_set_style_bg_image_src(saver, backdrop, 0);
	}
}

// The metadata field's ink, by palette. Amber takes the cream, amber and
// signal-orange roles sampled from its reference Now Playing screen, and OLED
// the white and grey steps of its own deck; Tokyo Night takes the violet it
// gives the deck's track name, the cyan it gives the artist line and its own
// accent blue for the time. Dark and Light have no
// field of their own: they keep the light type they have always worn over the
// blurred strip, whatever the page behind them is.
static void screensaver_refresh_theme(void) {
	if (!saver) {
		return;
	}

	bool mono = theme_is_monochrome(); // Amber on Black and OLED
	bool tokyo = theme_is_tokyo_night();
	bool field = mono || tokyo;

	// The page behind everything. Where there is a field it is the palette's own
	// background -- both the ground the note sits on when a track has no artwork
	// and the colour the translucent field is read against; black otherwise,
	// which is what the blurred strip is built to sit on.
	lv_color_t page = field ? theme()->screen_bg : lv_color_black();
	lv_obj_set_style_bg_color(saver, page, 0);
	if (cover_box) {
		lv_obj_set_style_bg_color(cover_box, page, 0);
	}

	// What the words and the marks are drawn in. Over a field they take the
	// roles the palette's own Now Playing deck gives the same things: the
	// track name's colour for the title, the artist line's for the artist --
	// which is Amber's amber and Tokyo Night's cyan, the one place the two
	// palettes ask a different colour of the same role -- the accent for the
	// time and the deck's quiet chevron for the way out.
	lv_color_t ink_title = lv_color_white();
	lv_color_t ink_artist = lv_color_make(200, 200, 200);
	lv_color_t ink_clock = lv_color_white();
	lv_color_t ink_chevron = lv_color_white();
	if (field) {
		ink_title = theme_player_chrome_color(THEME_PLAYER_CHROME_PRIMARY);
		ink_artist = tokyo ? theme_semantic_color(THEME_SEMANTIC_CYAN)
						   : theme_player_chrome_color(THEME_PLAYER_CHROME_SECONDARY);
		ink_clock = theme()->accent;
		ink_chevron = theme_player_chrome_color(THEME_PLAYER_CHROME_CHEVRON);
	}

	if (cover_placeholder) {
		// The note in the middle of a screen with no artwork: the monochrome
		// palettes' secondary deck role, the palette's own secondary text
		// everywhere else.
		lv_obj_set_style_image_recolor(cover_placeholder,
									   mono ? theme_player_chrome_color(THEME_PLAYER_CHROME_SECONDARY)
											 : theme()->text_secondary,
									   0);
	}
	if (title_label) {
		lv_obj_set_style_text_color(title_label, ink_title, 0);
	}
	if (artist_label) {
		lv_obj_set_style_text_color(artist_label, ink_artist, 0);
	}
	if (clock_label) {
		lv_obj_set_style_text_color(clock_label, ink_clock, 0);
	}
	if (chevron_icon) {
		lv_obj_set_style_image_recolor(chevron_icon, ink_chevron, 0);
	}

	// A theme can change while this is on screen. Reapply the source-specific
	// field immediately, so a blurred frame never survives into a palette that
	// has one of its own.
	if (screensaver_is_visible()) {
		refresh_content();
	}
}

// Fill, and pictures from the folder: the frame is the whole artwork area,
// square-cornered, so whatever is drawn in it is cropped by the area's own
// edges, on the page colour.
static void frame_fill(void) {
	lv_obj_set_size(frame_box, lv_pct(100), lv_pct(100));
	lv_obj_set_style_radius(frame_box, 0, 0);
	lv_obj_set_style_clip_corner(frame_box, false, 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	set_page_backdrop(NULL);
}

// Whether a picture of the player's is a sleeve -- square, give or take the
// odd pixel of rounding -- rather than Immersive's crop to the screen's shape,
// which framed would be a slice of the record rather than the record.
static bool is_sleeve(const lv_image_dsc_t *pic) {
	int w = (int)pic->header.w, h = (int)pic->header.h;
	int longer = w > h ? w : h;
	int diff = w > h ? w - h : h - w;
	return w > 0 && h > 0 && diff * 50 <= longer;
}

// The Framed style. Nothing is decoded here either: the copy made for the
// frame is drawn when it has landed, the player's sleeve until then.
static void draw_framed(const lv_image_dsc_t *backdrop) {
	set_strip_height(DEFAULT_STRIP_H);
	request_frame();

	const lv_image_dsc_t *pic = NULL;
	if (frame_pic.pixels) {
		pic = &frame_pic.dsc;
	} else if (shown_cover && is_sleeve(shown_cover)) {
		pic = shown_cover;
	}

	if (pic) {
		// One pixel to one, or smaller -- never larger. The copy made here is
		// already the frame's size, or smaller when the file's picture is,
		// and that is drawn as it is. The player's sleeve can be a few pixels
		// short of the frame (156 against 160 on the V1), which is left as it
		// is for the moment it is up, or larger (the 480 px sleeve of the
		// regular panels), which is brought down to fit.
		int w = (int)pic->header.w, h = (int)pic->header.h;
		int32_t scale = LV_SCALE_NONE;
		if (w > frame_side || h > frame_side) {
			int32_t by_w = (LV_SCALE_NONE * frame_side) / w;
			int32_t by_h = (LV_SCALE_NONE * frame_side) / h;
			scale = by_w < by_h ? by_w : by_h;
		}
		// The frame is the picture's drawn size, so its rounded corners are
		// the picture's corners. Rounded down: a frame a pixel smaller than a
		// resampled picture crops that pixel, where one a pixel larger would
		// show a line of page down one side.
		int shown_w = (int)((w * scale) / LV_SCALE_NONE);
		int shown_h = (int)((h * scale) / LV_SCALE_NONE);
		lv_obj_set_size(frame_box, shown_w > 0 ? shown_w : 1, shown_h > 0 ? shown_h : 1);
		lv_obj_set_style_radius(frame_box, FRAME_RADIUS, 0);
		lv_obj_set_style_clip_corner(frame_box, true, 0);
		lv_image_set_src(cover_img, pic);
		lv_image_set_scale(cover_img, (uint32_t)scale);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	// The page: the blurred copy behind everything in Dark and Light when the
	// player has one the size of the screen, the page colour otherwise -- and
	// always the page colour in the palettes, whose page is a colour by design.
	bool blur = !metadata_over_art() && backdrop && (int)backdrop->header.w == screen_w &&
				(int)backdrop->header.h == screen_h;
	set_page_backdrop(blur ? backdrop : NULL);
	lv_obj_set_style_bg_opa(cover_box, blur ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	set_info_strip_backdrop(NULL); // framed: no strip of its own
}

// Draws whatever is currently held. Nothing is decoded here.
static void draw_from_folder(void) {
	frame_fill();
	set_strip_height(DEFAULT_STRIP_H);

	if (image_pic.pixels) {
		lv_image_set_src(cover_img, &image_pic.dsc);
		// Already built at screen size by cover_load_screensaver_images: no
		// zoom to apply, and none to leave over from the album source.
		lv_image_set_scale(cover_img, 256);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		// An empty folder, or a card that is not there. The note in the middle
		// of a black screen, which is what the album source shows for a track
		// with no artwork -- the same screen, missing the same thing.
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	set_info_strip_backdrop(image_strip.pixels ? &image_strip.dsc : NULL);
}

// The artwork is the player's own, already decoded for this track: borrowed
// for as long as the screensaver is up and dropped the moment it goes down,
// so a track change while it is hidden can never leave a stale pointer here.
// A picture from the folder is the other way round -- owned here, decoded here
// and freed here -- and so is the Framed style's copy of the artwork, which
// the player's sleeve only stands in for until it lands.
static void refresh_content(void) {
	device_state_t state;
	device_state_get(&state);

	// Recorded before anything is drawn from it, so what the tick above
	// compares against is exactly what ended up on screen. A live stream has no
	// file, so the station name stands in; without it every radio would look
	// identical to the tick and a change of station would never redraw.
	snprintf(shown_file, sizeof(shown_file), "%s",
			 state.live ? state.metadata.title : state.current_file);

	// The placeholder behind a missing cover says what kind of thing is
	// playing: a note for a track, a radio for a stream.
	if (cover_placeholder) {
		lv_image_set_src(cover_placeholder, state.live ? &icon_radio_player : &icon_music_note);
	}

	if (state.live) {
		scrolltext_set(title_label, state.metadata.title);
		scrolltext_set(artist_label, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		scrolltext_set(title_label, state.metadata.title[0] ? state.metadata.title
															: (slash ? slash + 1 : state.current_file));
		const char *artist = metadata_shown_artist(&state.metadata);
		scrolltext_set(artist_label, artist);
	} else {
		scrolltext_set(title_label, "");
		scrolltext_set(artist_label, "");
	}

	// Recorded whatever is being drawn: the tick above compares it to notice a
	// track change, and in Images mode a track change still has to redraw the
	// title -- it just must not change the picture.
	shown_cover = player_cover_image();

	if (source == SCREENSAVER_SOURCE_IMAGES) {
		draw_from_folder();
		refresh_clock();
		return;
	}

	const lv_image_dsc_t *backdrop = player_backdrop_image();
	if (framed_now()) {
		draw_framed(backdrop);
		refresh_clock();
		return;
	}

	// The strip first: in the regular themes the artwork is scaled into the
	// area above it. The two V1 palettes settle the same panel height, but
	// overlay it on a full-screen cover so their translucent field really has
	// the sleeve under it.
	frame_fill();
	// The player's blurred copy is as tall as whatever shows it, and one
	// arrangement shows it behind the whole screen: a strip that tall is the
	// screen, so that one keeps the default height and takes the top of the
	// picture instead. The two V1 palettes deliberately never borrow that
	// blurred copy -- their field is a colour, not a blur.
	int strip_from_backdrop = backdrop ? (int)backdrop->header.h : 0;
	set_strip_height(!metadata_over_art() && strip_from_backdrop > 0 && strip_from_backdrop < screen_h
														 ? strip_from_backdrop : DEFAULT_STRIP_H);
	const lv_image_dsc_t *cover = shown_cover;
	if (cover) {
		lv_image_set_src(cover_img, cover);
		// Fill the available artwork area, cropping the overflow: a letterboxed
		// cover with black bars would look like a bug, not a screensaver. Under
		// a field the whole screen is artwork, the field included.
		int image_h = metadata_over_art() ? screen_h : screen_h - strip_h;
		int32_t zoom_w = (256 * screen_w) / (int32_t)cover->header.w;
		int32_t zoom_h = (256 * image_h) / (int32_t)cover->header.h;
		lv_image_set_scale(cover_img, zoom_w > zoom_h ? zoom_w : zoom_h);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	// In Amber and Tokyo Night this clears the blurred artwork and puts the
	// palette's translucent field in its place.
	set_info_strip_backdrop(backdrop);

	refresh_clock();
}

// ---------------------------------------------------------------------------
// raising and dismissing
// ---------------------------------------------------------------------------

static void anim_y_cb(void *obj, int32_t v);

void screensaver_show(void) {
	if (!saver || !enabled || screensaver_is_visible()) {
		return;
	}

	collect_next();
	promote_next();
	collect_frame();
	refresh_content();
	lv_anim_delete(saver, anim_y_cb);
	lv_obj_set_y(saver, 0);
	lv_obj_set_hidden(saver, false);
	lv_obj_move_foreground(saver);
	lv_timer_resume(clock_timer);
}

// Everything that has to happen once it is really gone, whether it was
// dismissed by a tap, by a finished swipe, or by switching the option off.
static void finish_hide(void) {
	lv_obj_set_hidden(saver, true);
	lv_obj_set_y(saver, 0);
	lv_timer_pause(clock_timer);
	// Let go of the player's pixels: it frees them at the next track change,
	// and nothing here may still be pointing at them by then. The picture owned
	// here goes too -- about a megabyte, and the device is in use again, which
	// is exactly when that megabyte is worth having back.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	set_page_backdrop(NULL);
	drop_image();
	// The one being prepared goes too. It is another megabyte, the device is in
	// use again, and the next screen-off asks for a fresh one anyway.
	drop_next();
	// And the framed copy, for the same reason: the track may well change
	// before the next screen-off, which asks for its own.
	drop_frame();
	power_notify_activity(); // dismissing it counts as using the device
}

static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// Where the slide currently running is headed. Held explicitly rather than read
// back from the final coordinate: LVGL can finish an animation a pixel or four
// short of its end value, which would answer "did it leave?" with a no and
// leave the screensaver technically visible off-screen forever.
static bool sliding_out;

static void slide_done_cb(lv_anim_t *a) {
	(void)a;
	if (sliding_out) {
		finish_hide();
	} else {
		lv_obj_set_y(saver, 0); // exactly home, whatever the last frame did
	}
}

// Animates the last stretch after the finger lets go: up and away, or back
// down into place.
static void slide_to(int y) {
	lv_anim_delete(saver, anim_y_cb);
	sliding_out = (y != 0);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, saver);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(saver), y);
	lv_anim_set_duration(&a, DISMISS_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	lv_anim_start(&a);
}

bool screensaver_prepare(void) {
	if (!saver || !enabled) {
		return false;
	}
	// Called once as the panel goes dark and again as it lights up, and the two
	// do different work. Going dark is when the next picture is asked for: the
	// screen is about to be off, the loader thread has it to itself, and nobody
	// is waiting. Lighting up is when it is collected and swapped in -- no card,
	// no decode, nothing between the button and the backlight.
	bool waking = power_screen_is_on();

	if (screensaver_is_visible()) {
		collect_next();
		if (waking) {
			promote_next();
		}
		// Framed, the redraw below also asks for the framed copy as the panel
		// goes dark, if there is none yet -- and on the way back puts up the
		// one decoded meanwhile, before the first frame reaches the glass.
		collect_frame();
		refresh_content();
	} else {
		screensaver_show(); // collects and promotes on its way through
	}

	if (!waking) {
		request_next();
	}
	return screensaver_is_visible();
}

void screensaver_release_artwork(void) {
	if (!cover_img) {
		return;
	}
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Nothing here is the player's: it can free its artwork without this
		// screen losing anything, and blanking the picture would put a black
		// screen up for no reason.
		return;
	}

	// Whatever was on screen is gone; the tick above has to see a difference
	// so it puts the new track's picture up rather than leaving black.
	shown_cover = NULL;
	shown_file[0] = '\0';
	// Whatever happens next, nothing here may still be pointing at the
	// player's buffers when it frees them -- the page behind a framed sleeve
	// included, which is the player's blurred copy.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_hidden(cover_img, true);
	lv_obj_set_hidden(cover_placeholder, false);
	set_info_strip_backdrop(NULL);
	set_page_backdrop(NULL);
	// The player only lets go of a picture that is changing, so the framed
	// copy of it is out of date, and so is any copy still on its way. Both go;
	// the redraw asks for the next one once the player has it.
	drop_frame();
}

void screensaver_hide(void) {
	if (!saver || !screensaver_is_visible()) {
		return;
	}
	slide_to(-screen_h);
}

// The swipe up, following the finger the whole way: the screensaver slides
// with it, and the release decides whether it carries on off the top or falls
// back into place. Same feel as the control centre.
static void drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		lv_anim_delete(saver, anim_y_cb);
		tracking = true;
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dy = point.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (dy > -DRAG_ENGAGE_PX) {
				return; // not (yet) an upward drag
			}
			engaged = true;
		}
		// Only upward: dragging down does not pull it off the bottom.
		lv_obj_set_y(saver, dy < 0 ? dy : 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		// A quarter of the screen of travel and it goes; short of that it
		// falls back.
		slide_to(dy < -screen_h / 4 ? -screen_h : 0);
	}
}

void screensaver_init(gui_config_t *cfg) {
	screen_w = cfg->screen_width;
	screen_h = cfg->screen_height;
	strip_h = DEFAULT_STRIP_H;
	picture_max_w = bp_pick(V1_PICTURE_MAX_W, screen_w);
	picture_max_h = bp_pick(V1_PICTURE_MAX_H, screen_h);

	screensaver_set_card_root(cfg->sd_root_path);
	enabled = config_get_bool("screen", "screensaver", false);
	source = config_get_int("screen", "screensaver_source", SCREENSAVER_SOURCE_ALBUM) == SCREENSAVER_SOURCE_IMAGES
				 ? SCREENSAVER_SOURCE_IMAGES
				 : SCREENSAVER_SOURCE_ALBUM;
	style = config_get_int("screen", "screensaver_style", SCREENSAVER_STYLE_FILL) == SCREENSAVER_STYLE_FRAMED
				? SCREENSAVER_STYLE_FRAMED
				: SCREENSAVER_STYLE_FILL;
	cover_box_h = screen_h - strip_h;

	// The framed sleeve's side: the largest square the margins leave inside
	// the area over a default-height strip, even so it centres on whole pixels.
	int frame_w = screen_w - 2 * FRAME_MARGIN_SIDE;
	int frame_h = screen_h - DEFAULT_STRIP_H - 2 * FRAME_MARGIN_VERT;
	frame_side = (frame_w < frame_h ? frame_w : frame_h) & ~1;

	saver = lv_obj_create(lv_layer_top());
	lv_obj_set_size(saver, screen_w, screen_h);
	lv_obj_set_pos(saver, 0, 0);
	lv_obj_set_style_bg_color(saver, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(saver, 0, 0);
	lv_obj_set_style_radius(saver, 0, 0);
	lv_obj_set_style_pad_all(saver, 0, 0);
	lv_obj_set_scrollable(saver, false);
	lv_obj_set_clickable(saver, true);
	lv_obj_set_hidden(saver, true);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The artwork: everything above the strip in the regular themes, and the
	// whole screen under the two V1 palettes, centre-cropped to fill it either
	// way -- or, Framed, the area above the strip in every theme, with the
	// sleeve centred in it. Its fill is the page colour (see
	// screensaver_refresh_theme): what shows when a track has no artwork
	// behind the note, and round a framed sleeve.
	cover_box = lv_obj_create(saver);
	lv_obj_set_size(cover_box, screen_w, screen_h - strip_h);
	lv_obj_set_pos(cover_box, 0, 0);
	lv_obj_set_style_bg_color(cover_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cover_box, 0, 0);
	lv_obj_set_style_radius(cover_box, 0, 0);
	lv_obj_set_style_pad_all(cover_box, 0, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_set_scrollable(cover_box, false);
	lv_obj_set_event_bubble(cover_box, true);
	lv_obj_set_clickable(cover_box, false);

	// Nothing to show for a track with no artwork: the same note the player
	// puts in the same place, so the screensaver is recognisably the player
	// with the controls taken away rather than a different screen that has
	// gone wrong.
	cover_placeholder = lv_image_create(cover_box);
	lv_image_set_src(cover_placeholder, &icon_music_note);
	lv_obj_set_style_image_recolor_opa(cover_placeholder, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder);

	// What the artwork is drawn in. In Fill and for the folder's pictures it
	// is the whole cover box, and the picture is cropped by the box's edges;
	// in Framed it shrinks to the sleeve, centred, and takes the player's
	// rounded corners (see draw_framed). Clear, so the box's page colour -- or
	// the blurred page behind a framed sleeve -- is what shows around it.
	frame_box = lv_obj_create(cover_box);
	lv_obj_set_size(frame_box, lv_pct(100), lv_pct(100));
	lv_obj_center(frame_box);
	lv_obj_set_style_bg_opa(frame_box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(frame_box, 0, 0);
	lv_obj_set_style_shadow_width(frame_box, 0, 0);
	lv_obj_set_style_radius(frame_box, 0, 0);
	lv_obj_set_style_pad_all(frame_box, 0, 0);
	lv_obj_set_scrollable(frame_box, false);
	lv_obj_set_event_bubble(frame_box, true);
	lv_obj_set_clickable(frame_box, false);

	cover_img = lv_image_create(frame_box);
	lv_obj_center(cover_img);
	lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);

	// Metadata field: the player's blurred artwork in Dark and Light, a
	// translucent fill of the palette's own colour in the two V1 palettes (see
	// set_info_strip_backdrop).
	info_strip = lv_obj_create(saver);
	lv_obj_set_size(info_strip, screen_w, strip_h);
	lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(info_strip, 0, 0);
	lv_obj_set_style_radius(info_strip, 0, 0);
	lv_obj_set_style_pad_hor(info_strip, cfg->padding + 6, 0);
	lv_obj_set_style_pad_ver(info_strip, bp_pick(8, 18), 0);
	lv_obj_set_style_pad_gap(info_strip, 2, 0);
	lv_obj_set_scrollable(info_strip, false);
	lv_obj_set_event_bubble(info_strip, true);
	lv_obj_set_clickable(info_strip, false);
	lv_obj_set_flex_flow(info_strip, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// None of the ink is set here. What the words and the marks are drawn in
	// depends on the palette -- Amber's cream and signal orange, Tokyo Night's
	// violet, cyan and blue, the light type Dark and Light have always worn over
	// the blurred strip -- and it is all written in screensaver_refresh_theme(),
	// which runs at the end of this function and again on every theme change.
	// One place, so the two can never disagree.
	title_label = lv_label_create(info_strip);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_set_style_text_font(title_label, &font_ui_28, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(info_strip);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(artist_label);

	// The time, in the largest figures the screen has.
	clock_label = lv_label_create(info_strip);
	lv_obj_set_style_text_font(clock_label, &font_ui_72, 0);
	lv_obj_set_style_pad_top(clock_label, bp_pick(2, 6), 0);

	// The way out, spelled out: swipe up. A sign, not a button -- tapping it
	// does nothing, since the screensaver is only dismissed by dragging -- so it
	// is not clickable and a finger landing on it talks to the saver underneath,
	// where the drag gesture lives.
	lv_obj_t *chevron_btn = lv_obj_create(saver);
	lv_obj_set_size(chevron_btn, bp_pick(48, 80), bp_pick(30, 52));
	lv_obj_align(chevron_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
	lv_obj_set_style_bg_opa(chevron_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chevron_btn, 0, 0);
	lv_obj_set_style_shadow_width(chevron_btn, 0, 0);
	lv_obj_set_style_pad_all(chevron_btn, 0, 0);
	lv_obj_set_clickable(chevron_btn, false);
	lv_obj_set_scrollable(chevron_btn, false);

	chevron_icon = lv_image_create(chevron_btn);
	lv_image_set_src(chevron_icon, &icon_chevron_up);
	lv_obj_set_style_image_recolor_opa(chevron_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(chevron_icon, LV_OPA_70, 0);
	lv_obj_center(chevron_icon);

	// One second, not ten: the clock only needs a minute's resolution, but the
	// same tick is what notices a track change, and a screensaver that shows
	// the previous song for ten seconds is a screensaver that looks broken.
	clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
	lv_timer_pause(clock_timer);
	// It is drawn into the framebuffer before the blank so the next wake opens
	// on it -- but with the panel off nothing is scanned out, so ticking it
	// once a second all night buys nothing.
	power_pause_in_standby(clock_timer);
	theme_register_refresh(screensaver_refresh_theme);
	screensaver_refresh_theme();
}
