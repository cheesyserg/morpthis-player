#include "topbar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/theme.h"
#include "src/system/remote/airplay.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/device/led.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/device/power.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/device/usb.h"
#include "src/system/audio/usbdac.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"

// The battery bitmaps are rendered from 24x24 SVGs at this size. The shell is
// drawn by the icon; the charge level is a plain rectangle behind it, showing
// through the icon's transparent middle.
#define BATTERY_ICON_SIZE 38
#define COMPACT_STATUS_ICON_SIZE 18
#define COMPACT_BATTERY_ICON_SIZE 20

static bool topbar_compact;
static int battery_icon_size = BATTERY_ICON_SIZE;

// The inner cavity of the battery shell, in the SVG's 24x24 coordinates: the
// body rect runs 2..18 with a 2px stroke centred on it, so the hole is 3..17
// across and 7..17 down.
#define BATTERY_CAVITY_X 3
#define BATTERY_CAVITY_Y 7
#define BATTERY_CAVITY_W 14
#define BATTERY_CAVITY_H 10

// The charging shell is the same body with a notch cut top and bottom for the
// bolt to pass through, so any fill drawn inside it leaks through the openings
// as a stripe above and below the outline. While charging the level bar is
// therefore omitted: the bolt says what is happening and the percentage beside
// it says how far along.

// Below this the fill turns red instead of green.
#define BATTERY_LOW_PERCENT 15

// Where the volume glyph changes: silent, quiet, loud.
#define VOLUME_LOW_PERCENT 1
#define VOLUME_HIGH_PERCENT 55

// A radio that is on but connected to nothing is drawn at this opacity; full
// opacity means something is on the other end.
#define RADIO_IDLE_OPA LV_OPA_40

// The radios get their own timer rather than riding the battery poll: five
// seconds is far too long to notice a network dropping, and reading two
// mutex-guarded structs costs nothing.
#define RADIO_POLL_MS 2000

// The jacks, the USB-C port and the charger. Fast enough that the glyph changes
// while the plug is still going in, which is the only rate that reads as
// "immediately".
#define JACK_POLL_MS 250

// How many of those polls have to agree before the charge state is acted on.
#define CHARGE_SETTLE_POLLS 3

// How far down a press has to end, with no drag having started, for the pull on
// the status bar to count as a flick and open the control centre anyway.
// Comfortably more than the wobble of a tap.
#define FLICK_OPEN_PX 24

static lv_obj_t *top_bar;
static lv_obj_t *bat_widget;
static lv_obj_t *bat_fill;
static lv_obj_t *bat_shell;
static lv_obj_t *bat_bolt;
static lv_obj_t *bat_label;
static lv_obj_t *vol_label;
static lv_obj_t *vol_icon;
static lv_obj_t *hp_icon; // headphone jack indicator: hidden / theme / gold
static lv_obj_t *play_icon;		 // play/pause indicator: hidden when nothing is loaded
static lv_obj_t *library_icon;	 // Detect changes at work, or just done
static lv_timer_t *library_timer; // takes the check glyph away
static lv_obj_t *sonixlink_icon; // shown while a phone is driving the player
static void refresh_play_icon(const device_state_t *state);
static void refresh_sonixlink_icon(void);
static lv_obj_t *clock_label;
static lv_obj_t *bt_icon;   // bluetooth, leftmost of the right-hand group
static lv_obj_t *wifi_icon; // wifi, between bluetooth and the charge percentage
static lv_obj_t *container_left;  // volume group (and the clock, when "left")
static lv_obj_t *container_right; // battery group (and the clock, when "right")
static lv_timer_t *battery_timer;
static lv_timer_t *jack_timer;
static int last_jack_state = -1;
static int last_usb_audio_state = -1;

// The last battery reading painted. Kept here rather than inside the poll so a
// theme switch can throw it away: the poll only repaints when the reading has
// moved, and new colours are not a new reading.
static int last_battery_percent = -2;
static int last_battery_charging = -1;
static lv_timer_t *clock_timer;
static lv_timer_t *radio_timer;

// Cavity geometry scaled to BATTERY_ICON_SIZE, worked out once at init.
static int cavity_x, cavity_y, cavity_w, cavity_h;

static void compact_status_icon(lv_obj_t *icon, const lv_image_dsc_t *source) {
	if (!topbar_compact || !icon || !source) {
		return;
	}

	// Transforming alone only changes the drawing; LVGL flex would still
	// reserve the source glyph's 26--34 px box. Give every glyph a real 18 px
	// layout box and derive the scale from its source, so Wi-Fi, volume and the
	// smaller playback glyph all align without crowding the centred clock.
	int max_side = LV_MAX((int)source->header.w, (int)source->header.h);
	lv_obj_set_size(icon, COMPACT_STATUS_ICON_SIZE, COMPACT_STATUS_ICON_SIZE);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
	if (max_side > 0) {
		lv_image_set_scale(icon, (uint32_t)(LV_SCALE_NONE * COMPACT_STATUS_ICON_SIZE / max_side));
	}
}

// Parses the sysfs capacity string. Returns -1 when it is not a number: the
// host build has no battery and its reader returns "!!".
static int parse_percent(const char *text) {
	if (!text || text[0] < '0' || text[0] > '9')
		return -1;

	int value = atoi(text);
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	return value;
}

// Repaints the battery indicator: level as the width of the coloured bar
// inside the shell, plus the charging bolt on top when the charger is in.
static void update_battery_indicator(int percent, bool charging) {
	lv_image_set_src(bat_shell, charging ? &icon_battery_charging_body : &icon_battery);

	if (charging) {
		lv_obj_set_hidden(bat_bolt, false);
	} else {
		lv_obj_set_hidden(bat_bolt, true);
	}

	if (percent < 0) {
		// Unknown level: empty shell, with nothing to fill in.
		lv_obj_set_hidden(bat_fill, true);
		lv_obj_set_style_image_recolor(bat_shell, theme()->text_secondary, 0);
		lv_label_set_text(bat_label, "--%");
		return;
	}

	lv_label_set_text_fmt(bat_label, "%d%%", percent);

	lv_obj_set_style_image_recolor(bat_shell,
								   theme_uses_function_glyphs() ? theme_semantic_color(THEME_SEMANTIC_GREEN) : theme()->text_primary, 0);

	if (charging) {
		lv_obj_set_hidden(bat_fill, true);
		return;
	}

	lv_obj_set_hidden(bat_fill, false);

	int width = (cavity_w * percent) / 100;
	if (width < 3 && percent > 0)
		width = 3; // a nearly flat battery must still read as a red sliver

	lv_obj_set_pos(bat_fill, cavity_x, cavity_y);
	lv_obj_set_size(bat_fill, width, cavity_h);

	lv_color_t color;
	if (theme_uses_function_glyphs()) {
		color = (percent <= BATTERY_LOW_PERCENT) ? theme_semantic_color(THEME_SEMANTIC_RED)
														  : theme_semantic_color(THEME_SEMANTIC_GREEN);
	} else {
		color = (percent <= BATTERY_LOW_PERCENT) ? lv_color_make(220, 60, 50) : lv_color_make(60, 190, 90);
	}
	lv_obj_set_style_bg_color(bat_fill, color, 0);
}

// Redraws the clock. Before the time has been set there is nothing truthful to
// show, so it reads as blank rather than as 1970.
void topbar_refresh_clock(void) {
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

static void clock_timer_cb(lv_timer_t *timer) {
	(void)timer;
	topbar_refresh_clock();
}

static void remember_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// Correct the wall clock against the RTC, or the boot clock, before writing
	// it down, so the remembered value is right and the bar stops drifting
	// behind between wakes.
	if (clock_resync()) {
		topbar_refresh_clock();
	}
	clock_remember();
}

// Amber on Black and OLED map the battery's semantic colours to their accent
// (signal orange, white). Give these two status glyphs that same colour; in
// every other theme they continue to inherit the regular icon style.
static void refresh_status_icon_color(lv_obj_t *icon) {
	if (!icon) {
		return;
	}

	if (theme_is_monochrome()) {
		lv_obj_set_style_image_recolor(icon, theme_semantic_color(THEME_SEMANTIC_GREEN), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	} else {
		lv_obj_remove_local_style_prop(icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
}

// The volume number is held at the width of the widest reading it can show, so
// the glyphs after it in the left group -- jack, playback, Detect changes,
// SonixLink -- stay put while the level moves. Sized to its text, the label
// shoved them sideways whenever the level gained or lost a digit, and in a face
// with proportional figures on most steps in between: in MiSans at the V1's
// 14 px a 1 is six pixels wide and a 0 nine, so the jack glyph took a dozen
// different positions on the way from 0 to 100.
//
// Measured, not written down: the face and the text size are both settings and
// a card font can be any width, so this runs again after either changes. Every
// reading from 0 to 100 is tried rather than three of the widest digit, which
// would hold room for an 888 the scale never reaches -- three pixels more in
// MiSans, and the V1's bar has none to spare.
//
// A minimum width rather than a fixed one: the label stays sized to its text,
// so a measure that came up short would let it grow, never wrap the number onto
// a second line. The number keeps to the side of the glyph that says what it
// means; the spare room opens up after it.
static void fit_volume_label(void) {
	if (!vol_label) {
		return;
	}

	const lv_font_t *font = lv_obj_get_style_text_font(vol_label, LV_PART_MAIN);
	int32_t letter_space = lv_obj_get_style_text_letter_space(vol_label, LV_PART_MAIN);
	int32_t widest = 0;
	for (int percent = 0; percent <= 100; percent++) {
		char text[8];
		snprintf(text, sizeof(text), "%d", percent);
		lv_point_t size;
		lv_text_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x > widest) {
			widest = size.x;
		}
	}
	lv_obj_set_style_min_width(vol_label, widest, 0);
}

// Volume, as a number with the glyph that matches its level.
void topbar_refresh_volume(int percent) {
	if (!vol_label) {
		return;
	}

	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	lv_label_set_text_fmt(vol_label, "%d", percent);

	// Keep the main-screen volume number white at every level.
	lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);

	const lv_image_dsc_t *icon = &icon_volume_high;
	if (percent < VOLUME_LOW_PERCENT) {
		icon = &icon_volume_mute;
	} else if (percent < VOLUME_HIGH_PERCENT) {
		icon = &icon_volume_low;
	}
	lv_image_set_src(vol_icon, icon);
	refresh_status_icon_color(vol_icon);
}

// Shows or hides the headphone glyph to match what is in the jacks. Four times
// a second, off jack_timer, so the glyph changes while the plug is still going
// in. A forced refresh is also used on wake so the first frame cannot carry an
// output icon left over from before the screen went dark.
static void refresh_headphone_icon(bool force) {
	int state = headphone_jack_state();

	// The same poll is where line out finds out that its jack has gone: the
	// mode holds the output at a fixed level, and that level does not belong to
	// whatever is plugged in next.
	lineout_check_jack();

	// And the USB-C port, on the same beat: a DAC or a pair of USB-C
	// headphones appearing means playback has somewhere else to go.
	usbaudio_poll();

	int usb = usbaudio_active() ? 1 : 0;
	bool changed = state != last_jack_state || usb != last_usb_audio_state;

	if (changed) {
		// Something left one of the sockets. Pausing belongs here and not
		// further down because this is the only code that remembers what was
		// plugged in a quarter of a second ago.
		//
		// -1 is the first poll after boot, where there is no "before" to have
		// left. And only the output actually carrying the music counts: with a
		// DAC on the port or headphones on the radio, the jacks are not what
		// anyone is listening to, and pulling a cable out of an unused socket
		// must not stop the album.
		if (last_usb_audio_state == 1 && usb == 0) {
			player_output_unplugged("the USB-C DAC");
		} else if (last_jack_state > JACK_NONE && state == JACK_NONE && !usb) {
			char device[160];
			audio_get_output_device(device, sizeof(device));
			if (!device[0] || strcmp(device, "default") == 0) {
				player_output_unplugged(last_jack_state == JACK_BALANCED ? "the 4.4 mm jack" : "the 3.5 mm jack");
			}
		}

		last_jack_state = state;
		last_usb_audio_state = usb;
	}

	// A normal poll can return when the hardware answer is unchanged. The forced
	// wake refresh bypasses this cache and redraws the current output state
	// before the first frame is presented.
	if (!hp_icon) {
		return;
	}
	if (!changed && !force) {
		return;
	}

	// With a DAC on the port the sockets are not what the sound comes out of,
	// whatever is plugged into them.
	if (usb) {
		lv_image_set_src(hp_icon, &icon_usbaudioout);
		if (theme_uses_function_glyphs()) {
			lv_obj_set_style_image_recolor(hp_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
			lv_obj_set_style_image_recolor_opa(hp_icon, LV_OPA_COVER, 0);
		} else {
			lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR, 0);
			lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		}
		lv_obj_set_hidden(hp_icon, false);
		return;
	}
	lv_image_set_src(hp_icon, &icon_headphones);

	if (state == JACK_NONE) {
		lv_obj_set_hidden(hp_icon, true);
		return;
	}

	lv_obj_set_hidden(hp_icon, false);
	if (state == JACK_BALANCED) {
		// Gold marks the balanced output; the monochrome palettes use their
		// accent rather than introducing a second hue.
		lv_obj_set_style_image_recolor(hp_icon,
									   theme_is_monochrome() ? theme()->accent : lv_color_make(212, 175, 55), 0);
		lv_obj_set_style_image_recolor_opa(hp_icon, LV_OPA_COVER, 0);
	} else if (theme_uses_function_glyphs()) {
		// Cyan marks the listening/output glyph in Tokyo Night.
		lv_obj_set_style_image_recolor(hp_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
		lv_obj_set_style_image_recolor_opa(hp_icon, LV_OPA_COVER, 0);
	} else {
		// The single-ended jack keeps the original theme's icon colour.
		lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
}

// Called before the wake repaint so a cable removed while the screen was dark
// cannot leave a stale headphone glyph in the first visible frame.
void topbar_refresh_audio_outputs(void) { refresh_headphone_icon(true); }

// The codec the Bluetooth link is carrying, in whichever direction it runs:
// headphones being fed from here, or a phone sending to this player. Both are
// cache reads, so this is safe on the poll. Empty when nothing is connected.
static void bt_current_codec(char *out, size_t size) {
	out[0] = '\0';

	if (bluetooth_audio_active()) {
		char offered[BT_MAX_CODECS][BT_CODEC_MAX];
		char selected[BT_CODEC_MAX] = "";
		bluetooth_get_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected));
		snprintf(out, size, "%s", selected);
		return;
	}

	bt_stream_t stream;
	if (bluetooth_receiver_stream(&stream)) {
		snprintf(out, size, "%s", stream.codec);
	}
}

// The Bluetooth glyph wears the colour of the codec family in use, out of the
// Adwaita palette the rest of the interface is drawn from: orange for SBC,
// green for AAC, purple for aptX, blue for LDAC, each in the light or dark
// variant the palette gives for it.
//
// By family and not by name: aptX-HD is aptX and SBC-XQ is SBC, and a codec
// this has never heard of gets no colour at all rather than a wrong one.
// Compared on letters and digits only, in lower case, the way bluetooth.c
// compares them -- bluealsa spells it "aptX-HD" and a config file might say
// "aptx_hd".
static bool bt_codec_color(const char *codec, lv_color_t *out) {
	char key[BT_CODEC_MAX];
	size_t used = 0;
	for (const char *p = codec; *p && used + 1 < sizeof(key); p++) {
		if (isalnum((unsigned char)*p)) {
			key[used++] = (char)tolower((unsigned char)*p);
		}
	}
	key[used] = '\0';

	bool dark = theme_is_dark();
	if (strncmp(key, "ldac", 4) == 0) {
		*out = dark ? lv_color_make(0x62, 0xA0, 0xEA) : lv_color_make(0x1A, 0x5F, 0xB4);
	} else if (strncmp(key, "aptx", 4) == 0) {
		*out = dark ? lv_color_make(0xDC, 0x8A, 0xDD) : lv_color_make(0x81, 0x3D, 0x9C);
	} else if (strncmp(key, "aac", 3) == 0) {
		*out = dark ? lv_color_make(0x57, 0xE3, 0x89) : lv_color_make(0x26, 0xA2, 0x69);
	} else if (strncmp(key, "sbc", 3) == 0) {
		*out = dark ? lv_color_make(0xFF, 0xA3, 0x48) : lv_color_make(0xC6, 0x46, 0x00);
	} else {
		return false;
	}
	return true;
}

// The two radios, immediately left of the charge percentage: bluetooth first,
// then wifi, so switching one on never moves the other. Each is hidden while
// its radio is off and drawn at RADIO_IDLE_OPA while the radio is up with
// nothing on the other end, which is the difference between the wifi being on
// and the wifi being connected.
void topbar_refresh_radios(void) {
	if (wifi_icon) {
		wifi_status_t status;
		wifi_get_status(&status);

		// The switch, not the worker's state, decides whether the glyph is there
		// at all: flipping the toggle has to show something immediately, and
		// bringing the radio up takes a second or two during which the status
		// still reads OFF.
		if (!wifi_get_enabled()) {
			lv_obj_set_hidden(wifi_icon, true);
		} else {
			lv_obj_set_hidden(wifi_icon, false);

			if (status.state == WIFI_STATE_CONNECTED) {
				// Connected: the arc shows the signal strength.
				const lv_image_dsc_t *glyph = &icon_wifi_max;
				if (status.bars <= 0) {
					glyph = &icon_wifi_zero;
				} else if (status.bars == 1) {
					glyph = &icon_wifi_low;
				} else if (status.bars == 2) {
					glyph = &icon_wifi_high;
				}
				lv_image_set_src(wifi_icon, glyph);
				lv_obj_set_style_image_opa(wifi_icon, LV_OPA_COVER, 0);
			} else {
				// On but attached to nothing: the full arc, faded. The empty
				// glyph is a single dot at this size and reads as a speck of dust
				// rather than a radio.
				lv_image_set_src(wifi_icon, &icon_wifi_max);
				lv_obj_set_style_image_opa(wifi_icon, RADIO_IDLE_OPA, 0);
			}
		}
	}

	if (wifi_icon) {
		if (theme_uses_function_glyphs()) {
			lv_obj_set_style_image_recolor(wifi_icon, theme_semantic_color(THEME_SEMANTIC_CYAN), 0);
			lv_obj_set_style_image_recolor_opa(wifi_icon, LV_OPA_COVER, 0);
		} else {
			lv_obj_remove_local_style_prop(wifi_icon, LV_STYLE_IMAGE_RECOLOR, 0);
			lv_obj_remove_local_style_prop(wifi_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		}
	}

	if (bt_icon) {
		// Same rule as the wifi glyph, and it matters more here: bringing the
		// Bluetooth stack back up takes the best part of ten seconds, and a
		// status bar that stays empty for all of them looks like the switch did
		// nothing.
		if (!bluetooth_get_enabled()) {
			lv_obj_set_hidden(bt_icon, true);
		} else {
			lv_obj_set_hidden(bt_icon, false);
			lv_obj_set_style_image_opa(bt_icon,
									   bluetooth_get_state() == BT_STATE_CONNECTED ? LV_OPA_COVER : RADIO_IDLE_OPA, 0);

			// A local colour while a codec is known, and the shared icon style
			// back when it is not: a radio that is merely on has no codec to
			// name, and the glyph belongs to the theme again.
			char codec[BT_CODEC_MAX];
			lv_color_t color;
			bt_current_codec(codec, sizeof(codec));
			// Amber on Black and OLED stay monochrome even when codec metadata
			// is known; Tokyo Night keeps its codec-family colors.
			if (codec[0] && !theme_is_monochrome() && bt_codec_color(codec, &color)) {
				lv_obj_set_style_image_recolor(bt_icon, color, 0);
				lv_obj_set_style_image_recolor_opa(bt_icon, LV_OPA_COVER, 0);
			} else if (theme_uses_function_glyphs()) {
				lv_obj_set_style_image_recolor(bt_icon, theme_semantic_color(THEME_SEMANTIC_BLUE), 0);
				lv_obj_set_style_image_recolor_opa(bt_icon, LV_OPA_COVER, 0);
			} else {
				lv_obj_remove_local_style_prop(bt_icon, LV_STYLE_IMAGE_RECOLOR, 0);
				lv_obj_remove_local_style_prop(bt_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
			}
		}
	}

	// The phone rides the same poll: it is a radio too, and two seconds is soon
	// enough for something that only changes when someone opens the app.
	refresh_sonixlink_icon();
}

// The SonixLink logo: there while a phone has asked for something recently, gone
// when nothing has. Tied to the traffic and not to the switch, which says only
// that the player is listening -- the bar reports what is happening, not what is
// possible.
static void refresh_sonixlink_icon(void) {
	if (!sonixlink_icon) {
		return;
	}
	if (sonixlink_is_connected()) {
		lv_obj_set_hidden(sonixlink_icon, false);
	} else {
		lv_obj_set_hidden(sonixlink_icon, true);
	}
}

// How long the check glyph stays once a Detect changes run is over.
#define LIBRARY_CHECKED_MS 4000

static void library_timer_cb(lv_timer_t *timer) {
	(void)timer;
	library_timer = NULL; // a one-shot timer, deleted by LVGL after this call
	lv_obj_set_hidden(library_icon, true);
}

void topbar_set_library_check(topbar_library_t state) {
	if (!library_icon) {
		return;
	}
	if (library_timer) {
		lv_timer_delete(library_timer);
		library_timer = NULL;
	}
	switch (state) {
	case TOPBAR_LIBRARY_IDLE:
		lv_obj_set_hidden(library_icon, true);
		break;
	case TOPBAR_LIBRARY_CHECKING:
		lv_image_set_src(library_icon, &icon_library_checking);
		lv_obj_set_hidden(library_icon, false);
		break;
	case TOPBAR_LIBRARY_CHECKED:
		lv_image_set_src(library_icon, &icon_library_checked);
		lv_obj_set_hidden(library_icon, false);
		library_timer = lv_timer_create(library_timer_cb, LIBRARY_CHECKED_MS, NULL);
		lv_timer_set_repeat_count(library_timer, 1);
		break;
	}
}

static void radio_timer_cb(lv_timer_t *timer) {
	(void)timer;
	topbar_refresh_radios();
}

// The playback indicator: the play glyph while a track runs, the pause glyph
// while one is loaded but stopped, nothing at all when there is no track. It
// takes the same colour as everything else in the bar rather than the accent,
// which would make it shout.
void topbar_refresh_playback(void) {
	device_state_t state;
	device_state_get(&state);
	refresh_play_icon(&state);
}

static void refresh_play_icon(const device_state_t *state) {
	if (!play_icon) {
		return;
	}

	// The AirPlay glyph replaces play/pause rather than sitting beside it. Only
	// one thing comes out of the DAC, so showing both would claim two, and the
	// play triangle would be wrong anyway: the local track is stopped while the
	// phone is the one playing.
	//
	// The swap happens only while audio is actually arriving, not as soon as the
	// receiver is switched on: AirPlay stays enabled on its own (see
	// quickpanel.c), so enabled and playing are different states and the bar has
	// to report the second.
	airplay_state_t ap;
	airplay_get_state(&ap);
	if (ap.playing) {
		lv_image_set_src(play_icon, &icon_airplay_status);
		lv_obj_remove_local_style_prop(play_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_set_hidden(play_icon, false);
		return;
	}

	if (state->status == AUDIO_STATUS_PLAYING) {
		lv_image_set_src(play_icon, &icon_play_status);
	} else if (state->current_file[0]) {
		lv_image_set_src(play_icon, &icon_pause_status);
	} else {
		lv_obj_set_hidden(play_icon, true);
		return;
	}

	// Amber on Black and OLED keep this status glyph in the battery's colour.
	// Other themes retain the Bluetooth-blue route indicator while it plays.
	if (theme_is_monochrome()) {
		refresh_status_icon_color(play_icon);
	} else if (state->status == AUDIO_STATUS_PLAYING && bluetooth_audio_active()) {
		// Fixed Bluetooth blue rather than the accent: it means wireless, not active.
		lv_obj_set_style_image_recolor(play_icon,
									   theme_uses_function_glyphs() ? theme_semantic_color(THEME_SEMANTIC_BLUE)
														 : lv_color_make(0, 122, 255),
									   0);
	} else {
		lv_obj_remove_local_style_prop(play_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(play_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
	lv_obj_set_hidden(play_icon, false);
}

// The things noticed with the hands, on their own fast timer: what is in the
// jacks, what is on the USB-C port, and whether the charger is in. Everything
// here returns at once when nothing has changed.
static void jack_timer_cb(lv_timer_t *timer) {
	(void)timer;

	refresh_headphone_icon(false);

	// The charger, on the same beat and for the same reason as the jacks: it is
	// something the user just did with their hands, and up to five seconds of a
	// battery icon that has not noticed reads as a cable that has not gone in.
	// The reads are a handful of small sysfs files; the repaint below is what
	// costs, and it only happens when the reading has actually moved.
	device_state_refresh_battery();

	device_state_t state;
	device_state_get(&state);
	int percent = parse_percent(state.battery_percent);

	// "Charging" out of the power supplies means a cable that is supplying,
	// which stays true long after the battery has stopped taking anything. What
	// the bolt and the red LED are for is a charge in progress, so the two
	// endings of one are taken out of it: the battery full, and the charger held
	// off at the configured limit. The cable is still in and the player still
	// runs off it -- there is simply nothing left to indicate.
	bool done = state.battery_charging && percent >= 0 && (percent >= 100 || power_charging_held());

	// One number for the whole answer, so the two halves can never disagree:
	// 0 no cable, 1 a cable with nothing going into the battery, 2 charging.
	int charge_state = !state.battery_charging ? 0 : (done ? 1 : 2);

	// A new answer has to come back the same way CHARGE_SETTLE_POLLS times
	// before it is acted on.
	//
	// The supplies do not read straight for a few seconds after the panel comes
	// back: the panel's rails are opened and closed at the AXP2101, and
	// /sys/class/power_supply/usb is that same PMIC, so the load step at the
	// unblank lands in the middle of what is being read. Unfiltered it is a bolt
	// appearing and vanishing four times a second, and -- the reason this is
	// here -- an LED pattern rewritten on every flip. The charging red is a
	// ramp programmed into the SGM31324, and a rewrite restarts the ramp, so
	// what comes out is a red that stutters for as long as the reading wobbles.
	//
	// Three polls is three quarters of a second, which still shows a cable going
	// in while the plug is still moving.
	static int settled = -1; // the charge state currently being acted on
	static int candidate = -1;
	static int candidate_polls;

	if (charge_state != candidate) {
		candidate = charge_state;
		candidate_polls = 1;
		if (settled >= 0 && candidate != settled) {
			static const char *const NAMES[] = {"no cable", "cable, not charging", "charging"};
			printf("battery: the charge reading says %s; waiting for it to settle\n", NAMES[candidate]);
		}
	} else if (candidate_polls < CHARGE_SETTLE_POLLS) {
		candidate_polls++;
	}
	if (settled < 0 || candidate_polls >= CHARGE_SETTLE_POLLS) {
		settled = candidate;
	}

	if (percent == last_battery_percent && settled == last_battery_charging) {
		return;
	}
	last_battery_percent = percent;
	last_battery_charging = settled;

	update_battery_indicator(percent, settled == 2);

	// And the LED, which says the same thing in the dark, from the same settled
	// answer: the pattern is never rewritten for a reading that is about to
	// change back. The cable goes with it -- a charge that has finished still
	// leaves the player on a charger, and that is what keeps the LED off the
	// darken-in-standby option.
	led_set_charging(settled == 2);
	led_set_on_charger(settled != 0);
}

// The periodic poll: battery, volume persistence, playback glyph and the
// status LED all ride this one timer.
static void timer_update_cb(lv_timer_t *timer) {
	(void)timer;

	// Both levels are written down from this poll rather than on every step of a
	// held volume key, so a long press costs one config write instead of fifty.
	// Which key each one goes to is alsa-controls' business: it is what knows
	// which profile is current, and it writes nothing when neither has moved.
	//
	// Ungated by "remember volume": that setting is about what the player comes
	// back to after a restart (see main.c), not about whether the two profiles
	// keep their own levels while it runs.
	volume_profile_persist();

	device_state_t state;
	device_state_get(&state);

	topbar_refresh_volume((int)state.volume);
	refresh_play_icon(&state);

	// The battery and the charge LED ride jack_timer instead, which runs often
	// enough for plugging the charger in to show at once. What is on this poll
	// is what nobody is standing over -- the playback colour, Wi-Fi transfer,
	// DAC mode.
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate,
						audio_stream_is_lossy(), audio_get_stream_bits(),
						!state.live && podcastcache_is_episode(state.current_file),
						!state.live && audio_get_dsd_multiple() > 0);
	// Wi-Fi transfer takes the same route: its pattern stays lit, screen off
	// included, for as long as the transfer is on. The switch rather than the
	// process, because this poll is five seconds apart and the server takes a
	// moment to come up and to die -- the light would lag the toggle by most of
	// a poll in both directions.
	led_set_wifi_transfer(wifitransfer_get_enabled());

	// And Bluetooth receiver mode, which like the transfer is a thing the
	// player is doing rather than something it is playing: the blue stays on
	// for as long as the mode is, screen off included.
	led_set_bt_receiver(btreceiver_is_active());

	// And DAC mode, which plays nothing through audio.c and so reads as a
	// stopped player above. Only while the cable is really there: the mode is
	// turned off from its own page and nowhere else, so a player unplugged and
	// pocketed would otherwise keep the LED lit against the standby setting.
	usbdac_state_t dac;
	usbdac_get_state(&dac);
	bool dac_live = dac.active && usb_vbus_present();
	led_set_dac(dac_live, dac_live && dac.streaming, dac.sample_rate);
}

// The edge of a side group that faces the clock: the right edge of the
// rightmost glyph on the left, or the left edge of the leftmost one on the
// right. Hidden glyphs take no room in the row and do not count. False when the
// group shows nothing at all.
static bool group_edge(lv_obj_t *group, bool right_edge, int32_t *edge) {
	bool found = false;
	uint32_t n = lv_obj_get_child_count(group);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(group, (int32_t)i);
		if (lv_obj_is_hidden(child)) {
			continue;
		}
		lv_area_t area;
		lv_obj_get_coords(child, &area);
		int32_t x = right_edge ? area.x2 : area.x1;
		if (!found || (right_edge ? x > *edge : x < *edge)) {
			*edge = x;
		}
		found = true;
	}
	return found;
}

// Where the centred clock goes. Centred on the bar whenever it fits there,
// which on the regular layout is nearly always. The V1's 240 px are another
// matter: the volume group with the jack and playback glyphs leaves the clock
// a dozen pixels or so, and the Detect changes glyph on top of those runs into
// the clock's first digit. So the clock steps aside, just far enough to keep
// the groups' own gap from the last glyph on the left. It keeps the same gap
// from the battery side and never runs into it, moving left instead when the
// radios and the charge reading crowd it from there. When there is not room for
// everything, the battery side keeps its gap and what overlap is left stays on
// the left.
//
// So what moves it is something beside it appearing, going or changing width,
// never the volume: the number, which changes all the time, holds one width
// (fit_volume_label()).
static void place_clock(void) {
	if (!clock_label || lv_obj_get_parent(clock_label) != top_bar || lv_obj_is_hidden(clock_label)) {
		return;
	}

	// The x LV_ALIGN_CENTER gives it, worked out the way lv_obj_refr_pos()
	// does: from the bar's content box, with the halves rounded separately.
	lv_area_t bar;
	lv_obj_get_content_coords(top_bar, &bar);
	int32_t width = lv_obj_get_width(clock_label);
	int32_t centred = bar.x1 + lv_area_get_width(&bar) / 2 - width / 2;

	int32_t gap = lv_obj_get_style_pad_column(container_left, 0);
	int32_t x = centred;
	int32_t edge;
	if (group_edge(container_left, true, &edge) && x < edge + 1 + gap) {
		x = edge + 1 + gap;
	}
	if (group_edge(container_right, false, &edge) && x + width > edge - gap) {
		x = edge - gap - width;
	}
	lv_obj_set_x(clock_label, x - centred); // does nothing when it has not moved
}

// Run after either side group has been laid out afresh (a glyph shown or
// hidden, a label grown or shrunk) and when the clock's own width changes with
// its digits or the face. Those are the moments the coordinates place_clock()
// reads are new; when the clock moves, LVGL lays it out again before anything
// is drawn.
static void clock_room_cb(lv_event_t *e) {
	(void)e;
	place_clock();
}

// Where the clock sits in the bar: centred on its own, first in the volume group
// on the left, or last in the battery group on the right. Re-parenting is all it
// takes, since the two side groups are flex rows and their layout absorbs it.
// Centred, it still steps aside when a side group crowds it: see
// place_clock().
void topbar_set_clock_position(int pos) {
	if (!clock_label) {
		return;
	}

	if (pos == TOPBAR_CLOCK_HIDDEN) {
		lv_obj_set_hidden(clock_label, true);
		return;
	}
	lv_obj_set_hidden(clock_label, false);

	if (pos == TOPBAR_CLOCK_LEFT) {
		lv_obj_set_parent(clock_label, container_left);
		lv_obj_move_to_index(clock_label, 0); // before the volume number
		lv_obj_set_align(clock_label, LV_ALIGN_DEFAULT);
	} else if (pos == TOPBAR_CLOCK_RIGHT) {
		lv_obj_set_parent(clock_label, container_right); // appended, so it lands right of the battery
		lv_obj_set_align(clock_label, LV_ALIGN_DEFAULT);
	} else {
		lv_obj_set_parent(clock_label, top_bar);
		lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
		// Back from hidden, or set again while already centred, the clock
		// leaves neither side group, and nothing would place it. Laying the
		// left group out again does: its event calls place_clock().
		lv_obj_mark_layout_as_dirty(container_left);
	}
}

// Re-runs the battery paint so its colours follow a theme switch. The reading
// is thrown away first, or the poll would see the same numbers as last time and
// keep the old colours.
//
// The radios come with it: the Bluetooth glyph carries a codec colour set by
// hand, which has a light and a dark variant and would otherwise keep the one
// from before the switch until the next poll.
static void topbar_refresh_theme(void) {
	last_battery_percent = -2;
	last_battery_charging = -1;
	if (jack_timer) {
		lv_timer_ready(jack_timer);
	}
	if (battery_timer) {
		lv_timer_ready(battery_timer);
	}
	if (radio_timer) {
		lv_timer_ready(radio_timer);
	}
	// These glyphs carry local semantic recolours rather than only the shared
	// theme style, so repaint them in the same frame as the palette switch.
	refresh_headphone_icon(true);
	topbar_refresh_radios();
	refresh_status_icon_color(vol_icon);
	topbar_refresh_playback();
}

// A downward drag starting on the status bar pulls the control panel in, and the
// panel follows the finger the whole way instead of snapping open the moment the
// gesture is recognised. A short drag released early falls back out.
static void topbar_drag_cb(lv_event_t *e) {
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
		// The control centre is reached by dragging this bar down, so this is the
		// path that has to honour the block. Guarding quickpanel_open() would not
		// cover it: this handler never calls it, it drives the panel directly
		// through quickpanel_drag_*().
		tracking = !quickpanel_is_open() && !quickpanel_blocked();
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (dy < 8) {
				return; // still a press, not a pull
			}
			engaged = true;
			quickpanel_drag_begin();
		}
		quickpanel_drag_update(dy);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged) {
			engaged = false;
			quickpanel_drag_end();
			return;
		}

		// A flick quick enough to be over between two reads of the touch panel
		// produces a press and a release with no PRESSING in between, so the
		// drag above never began and the panel stayed shut -- which from the
		// outside is a control centre that sometimes ignores a fast swipe. The
		// gesture did happen: the finger went down on the bar and came up this
		// far below where it landed.
		if (dy >= FLICK_OPEN_PX) {
			quickpanel_open();
		}
	}
}

// Nothing inside the bar takes a press; the bar does.
//
// Every press on the status bar has one meaning -- pull the control centre down
// -- and topbar_drag_cb() is on the bar itself, so a child that takes the press
// first is a strip of bar where the gesture does not start. LVGL hands the
// press to the topmost clickable object under the finger and nothing bubbles up
// unless asked to, so one clickable child is one dead patch.
//
// It is easy to add one by accident: lv_obj_create() returns a clickable object
// by default, while lv_image_create() and lv_label_create() do not. So this is
// done by walking what was built rather than by remembering at each call --
// the battery indicator is two plain objects and had been taking every press
// that landed on it.
static void topbar_clear_child_presses(lv_obj_t *obj) {
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		lv_obj_set_clickable(child, false);
		topbar_clear_child_presses(child);
	}
}

void topbar_init(gui_config_t *cfg) {
	if (top_bar != NULL) {
		return;
	}
	topbar_compact = cfg->screen_width < 320;
	battery_icon_size = topbar_compact ? COMPACT_BATTERY_ICON_SIZE : BATTERY_ICON_SIZE;

	// The bar lives on the top layer so it stays above every screen.
	top_bar = lv_obj_create(lv_layer_top());
	lv_obj_set_size(top_bar, cfg->screen_width, cfg->top_bar_height);
	lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_add_style(top_bar, &theme_style_panel, 0);
	lv_obj_set_style_border_width(top_bar, 0, 0);
	lv_obj_set_style_radius(top_bar, 0, 0);
	lv_obj_set_scrollable(top_bar, false);
	lv_obj_set_clickable(top_bar, true);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The default container padding would inset the bar's contents, so it goes.
	lv_obj_set_style_pad_left(top_bar, 0, 0);
	lv_obj_set_style_pad_right(top_bar, 0, 0);
	lv_obj_set_style_pad_top(top_bar, 0, 0);
	lv_obj_set_style_pad_bottom(top_bar, 0, 0);

	// The left-hand group: volume, then the jack and playback indicators, laid
	// out as a flex row so the two sides of the bar balance around the clock.
	container_left = lv_obj_create(top_bar);
	lv_obj_set_size(container_left, cfg->screen_width / 2, cfg->top_bar_height);
	lv_obj_align(container_left, LV_ALIGN_LEFT_MID, cfg->padding, 0);
	lv_obj_set_style_bg_opa(container_left, 0, 0);
	lv_obj_set_style_border_width(container_left, 0, 0);
	lv_obj_set_style_radius(container_left, 0, 0);
	lv_obj_set_style_pad_all(container_left, 0, 0);
	lv_obj_set_style_pad_gap(container_left, topbar_compact ? 2 : 8, 0);
	lv_obj_set_scrollable(container_left, false);
	// Not clickable, so presses reach the bar itself, whose drag handler pulls
	// the control panel down.
	lv_obj_set_clickable(container_left, false);
	lv_obj_set_flex_flow(container_left, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(container_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Glyph first, then the number: the icon says what the number means.
	vol_icon = lv_image_create(container_left);
	lv_image_set_src(vol_icon, &icon_volume_high);
	compact_status_icon(vol_icon, &icon_volume_high);
	lv_obj_add_style(vol_icon, &theme_style_icon, 0);
	refresh_status_icon_color(vol_icon);

	vol_label = lv_label_create(container_left);
	lv_label_set_text(vol_label, "--");
	lv_obj_add_style(vol_label, &theme_style_text, 0);
	lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(vol_label, topbar_compact ? &font_ui_14 : &font_ui_24, 0);
	// As wide as the widest reading, so nothing after it moves with the level;
	// measured again whenever the face or the text size changes.
	fit_volume_label();
	fonts_register_change(fit_volume_label);

	// The jack indicator: appears when headphones are plugged in, theme-coloured
	// for the 3.5 mm jack and gold for the 4.4 mm balanced one.
	hp_icon = lv_image_create(container_left);
	lv_image_set_src(hp_icon, &icon_headphones);
	compact_status_icon(hp_icon, &icon_headphones);
	lv_obj_add_style(hp_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(hp_icon, true);

	// What the player is doing, right of the jack indicator: play while a track
	// runs, pause while one is loaded and stopped, hidden when nothing is loaded.
	play_icon = lv_image_create(container_left);
	lv_image_set_src(play_icon, &icon_play_status);
	compact_status_icon(play_icon, &icon_play_status);
	lv_obj_add_style(play_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(play_icon, true);

	// Right of it, Detect changes while it runs and briefly once it is done
	// (topbar_set_library_check).
	library_icon = lv_image_create(container_left);
	lv_image_set_src(library_icon, &icon_library_checking);
	compact_status_icon(library_icon, &icon_library_checking);
	lv_obj_add_style(library_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(library_icon, true);

	// And right of that, the SonixLink logo while a phone is on the other end.
	// Alongside play/pause rather than in place of it: the phone is driving this
	// player, not taking the sound away from it, so what the DAC is doing stays
	// true and this only adds who is asking.
	sonixlink_icon = lv_image_create(container_left);
	lv_image_set_src(sonixlink_icon, &icon_sonixlink_status);
	compact_status_icon(sonixlink_icon, &icon_sonixlink_status);
	lv_obj_add_style(sonixlink_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(sonixlink_icon, true);

	// The right-hand group: radios, charge percentage and battery, packed to the
	// right edge.
	container_right = lv_obj_create(top_bar);
	lv_obj_set_size(container_right, cfg->screen_width, cfg->top_bar_height);
	lv_obj_align(container_right, LV_ALIGN_RIGHT_MID, -cfg->padding, 0);
	lv_obj_set_style_bg_opa(container_right, 0, 0);
	lv_obj_set_style_border_width(container_right, 0, 0);
	lv_obj_set_style_radius(container_right, 0, 0);
	// The container's default padding would push the battery about ten pixels
	// further in than the page padding everything else lines up with.
	lv_obj_set_style_pad_all(container_right, 0, 0);
	lv_obj_set_style_pad_gap(container_right, topbar_compact ? 2 : 8, 0);
	lv_obj_set_scrollable(container_right, false);
	lv_obj_set_clickable(container_right, false);
	lv_obj_set_flex_flow(container_right, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(container_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The radios come first in the group and so sit leftmost: bluetooth, then
	// wifi, then the charge percentage and the battery. Both start hidden;
	// topbar_refresh_radios() decides what is shown.
	bt_icon = lv_image_create(container_right);
	lv_image_set_src(bt_icon, &icon_bluetooth_status);
	compact_status_icon(bt_icon, &icon_bluetooth_status);
	lv_obj_add_style(bt_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(bt_icon, true);

	wifi_icon = lv_image_create(container_right);
	lv_image_set_src(wifi_icon, &icon_wifi_max);
	compact_status_icon(wifi_icon, &icon_wifi_max);
	lv_obj_add_style(wifi_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(wifi_icon, true);

	// Charge level as a number, immediately left of the shell.
	bat_label = lv_label_create(container_right);
	lv_label_set_text(bat_label, "--%");
	topbar_set_battery_percent(config_get_int("screen", "battery_percent", 1) != 0);
	lv_obj_add_style(bat_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(bat_label, topbar_compact ? &font_ui_14 : &font_ui_24, 0);

	// The battery indicator is stacked fill first, shell over it, charging bolt
	// above both. The cavity is sized by scaling both edges and taking the
	// difference rather than scaling width and height directly: rounding each
	// independently leaves the fill a pixel short of the bottom of the shell.
	//
	// Both edges are rounded to nearest. Flooring the near edge and taking the
	// ceiling of the far one, as this did, makes the fill up to two pixels
	// wider and taller than the hole it is supposed to sit in. At the 38 px
	// icon that is a hairline; at the V1's 20 px icon the fill is 13x10 in an
	// 11.7x8.3 cavity, and it spills out past the outline on three sides.
	cavity_x = (BATTERY_CAVITY_X * battery_icon_size + 12) / 24;
	cavity_y = (BATTERY_CAVITY_Y * battery_icon_size + 12) / 24;
	cavity_w = (((BATTERY_CAVITY_X + BATTERY_CAVITY_W) * battery_icon_size + 12) / 24) - cavity_x;
	cavity_h = (((BATTERY_CAVITY_Y + BATTERY_CAVITY_H) * battery_icon_size + 12) / 24) - cavity_y;

	bat_widget = lv_obj_create(container_right);
	lv_obj_set_size(bat_widget, battery_icon_size, battery_icon_size);
	lv_obj_set_style_bg_opa(bat_widget, 0, 0);
	lv_obj_set_style_border_width(bat_widget, 0, 0);
	lv_obj_set_style_pad_all(bat_widget, 0, 0);
	lv_obj_set_scrollable(bat_widget, false);

	bat_fill = lv_obj_create(bat_widget);
	lv_obj_set_pos(bat_fill, cavity_x, cavity_y);
	lv_obj_set_size(bat_fill, cavity_w, cavity_h);
	lv_obj_set_style_bg_color(bat_fill,
								   theme_uses_function_glyphs() ? theme_semantic_color(THEME_SEMANTIC_GREEN)
														 : lv_color_make(60, 190, 90),
								   0);
	lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bat_fill, 0, 0);
	lv_obj_set_style_radius(bat_fill, 1, 0);
	lv_obj_set_style_pad_all(bat_fill, 0, 0);
	lv_obj_set_scrollable(bat_fill, false);

	bat_shell = lv_image_create(bat_widget);
	lv_image_set_src(bat_shell, &icon_battery);
	lv_obj_set_style_image_recolor(bat_shell, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(bat_shell, LV_OPA_COVER, 0);
	if (topbar_compact) {
		lv_image_set_scale(bat_shell, (uint32_t)(LV_SCALE_NONE * battery_icon_size / BATTERY_ICON_SIZE));
		lv_obj_center(bat_shell);
	} else {
		lv_obj_set_pos(bat_shell, 0, 0);
	}

	bat_bolt = lv_image_create(bat_widget);
	lv_image_set_src(bat_bolt, &icon_battery_charging_bolt);
	lv_obj_set_style_image_recolor(bat_bolt,
								   theme_is_monochrome() ? theme()->accent : lv_color_make(245, 205, 60), 0);
	lv_obj_set_style_image_recolor_opa(bat_bolt, LV_OPA_COVER, 0);
	if (topbar_compact) {
		lv_image_set_scale(bat_bolt, (uint32_t)(LV_SCALE_NONE * battery_icon_size / BATTERY_ICON_SIZE));
		lv_obj_center(bat_bolt);
	} else {
		lv_obj_set_pos(bat_bolt, 0, 0);
	}
	lv_obj_set_hidden(bat_bolt, true);

	// Clock, centred on the bar unless a side group crowds it: see
	// place_clock() for that, and clock_room_cb() for when it is placed.
	clock_label = lv_label_create(top_bar);
	lv_obj_add_style(clock_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(clock_label, topbar_compact ? &font_ui_14 : &font_ui_24, 0);
	lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_event_cb(container_left, clock_room_cb, LV_EVENT_LAYOUT_CHANGED, NULL);
	lv_obj_add_event_cb(container_right, clock_room_cb, LV_EVENT_LAYOUT_CHANGED, NULL);
	lv_obj_add_event_cb(clock_label, clock_room_cb, LV_EVENT_SIZE_CHANGED, NULL);
	topbar_refresh_clock();
	topbar_set_clock_position((int)config_get_int("screen", "clock_pos", TOPBAR_CLOCK_CENTER));

	// Ten seconds keeps the minute from ever looking stale and costs nothing.
	clock_timer = lv_timer_create(clock_timer_cb, 10000, NULL);
	power_pause_in_standby(clock_timer); // a clock nobody can see is pure drain

	// The current time is written to the config, and checked against the RTC, so
	// a device whose RTC does not hold comes back up roughly right instead of at
	// 1970, and never further behind than one period.
	//
	// Every five minutes, and paused with the screen off: each round rewrites
	// device_config.ini on UBIFS, for a value that only matters if the battery
	// is pulled abruptly. What counts is writing the time when the screen goes
	// off, which power.c does by calling clock_remember() there (plus
	// clock_shutdown() on exit). The timer only earns its keep while somebody is
	// holding the device.
	lv_timer_t *remember_timer = lv_timer_create(remember_timer_cb, 300000, NULL);
	power_pause_in_standby(remember_timer);

	// What is left on the slow poll: the two volume levels written down, the
	// playback glyph, and the LED's playback colour. Nobody stands over any of
	// them with a cable in their hand.
	battery_timer = lv_timer_create(timer_update_cb, 5000, NULL);
	lv_timer_ready(battery_timer); // run immediately on startup

	// The jacks, the USB-C port and the charger keep their own, much faster
	// beat: on the five-second battery poll, plugging something in would take
	// up to five seconds to show, long enough to look broken to somebody still
	// holding the plug. A few small sysfs reads at this rate are cheap, and the
	// repainting only happens when what they find has changed.
	jack_timer = lv_timer_create(jack_timer_cb, JACK_POLL_MS, NULL);
	lv_timer_ready(jack_timer);
	// Slowed rather than stopped in standby: a jack pulled out with the screen
	// dark still has to take line out down with it, the LED still has to follow
	// the charger, and both icons have to be right before the panel comes back.
	power_slow_in_standby(jack_timer, 2000);
	// Slowed in standby, never stopped: this poll also tells led.c whether the
	// charger is in and re-evaluates the off-in-standby countdown. Stopped, the
	// LED would stay aqua for ever and never go red on a charger.
	power_slow_in_standby(battery_timer, 5000);

	// The radios keep their own faster beat: a network coming or going should
	// show up in a couple of seconds, not five.
	radio_timer = lv_timer_create(radio_timer_cb, RADIO_POLL_MS, NULL);
	lv_timer_ready(radio_timer);
	power_pause_in_standby(radio_timer);

	// Last, once everything that lives on the bar exists: see the note over the
	// function. The bar keeps its own CLICKABLE, which is what the drag hangs
	// off; only what is inside it gives presses up.
	topbar_clear_child_presses(top_bar);

	// The battery shell is recoloured by hand rather than by a style, so it needs
	// a repaint when the palette changes.
	theme_register_refresh(topbar_refresh_theme);
}

bool topbar_is_hidden(void) { return top_bar && lv_obj_is_hidden(top_bar); }

void topbar_set_battery_percent(bool shown) {
	if (!bat_label) {
		return;
	}
	// The V1 used to be forced off here, on the grounds that a percentage
	// would run under the centred clock. At font_ui_14 "100%" is about 32 px
	// and the compact battery is 20, so the right-hand group reaches roughly
	// 55 px in from the edge -- or 91 with both radios lit -- against a clock
	// that ends near 140. It fits, and the reading is worth more than the
	// margin. Setting > Display still turns it off.
	lv_obj_set_hidden(bat_label, !shown);
}

void topbar_bring_to_front(void) {
	if (top_bar) {
		lv_obj_move_foreground(top_bar);
	}
}

// Shows or hides the whole bar. The player page hides it so the album art can
// start at the very top of the screen.
void topbar_set_hidden(bool hidden) {
	if (!top_bar) {
		return;
	}

	if (hidden) {
		lv_obj_set_hidden(top_bar, true);
	} else {
		lv_obj_set_hidden(top_bar, false);
	}
}
