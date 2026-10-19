#include "theme.h"

#include "src/system/device/bootlogo.h"
#include "src/system/core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_style_t theme_style_screen;
lv_style_t theme_style_panel;
lv_style_t theme_style_card;
lv_style_t theme_style_card_pressed;
lv_style_t theme_style_text;
lv_style_t theme_style_text_dim;
lv_style_t theme_style_icon;
lv_style_t theme_style_switch;		   // switch track, off state
lv_style_t theme_style_switch_checked; // switch indicator, on state
lv_style_t theme_style_switch_knob_checked; // switch knob, on state
lv_style_t theme_style_switch_track_checked; // switch track, on state (OLED's ring)
lv_style_t theme_style_switch_disabled;      // switch, disabled (OLED only)
lv_style_t theme_style_accent_bg;	   // anything that must follow the accent
lv_style_t theme_style_slider_track;   // a slider's unfilled part

// GNOME Adwaita (libadwaita) colours, dark variant: window #242424, cards
// #303030 (white at 8%% over the window), accent blue #3584e4. Secondary text
// is white at 55%%, flattened against the card colour.
static theme_palette_t palette_dark = {
	.screen_bg = LV_COLOR_MAKE(36, 36, 36),
	.surface = LV_COLOR_MAKE(48, 48, 48),
	.surface_pressed = LV_COLOR_MAKE(64, 64, 64),
	.panel = LV_COLOR_MAKE(30, 30, 30),
	.cover_bg = LV_COLOR_MAKE(30, 30, 30),
	.text_primary = LV_COLOR_MAKE(255, 255, 255),
	.text_secondary = LV_COLOR_MAKE(160, 160, 160),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = true,
};

// Adwaita light: window #fafafa, cards white, pressed = black at 8%%, text
// black at 80%%. The accent is the same #3584e4 in both variants.
static theme_palette_t palette_light = {
	// The window is pushed further from white than stock Adwaita's #fafafa:
	// on this panel white cards on a #fafafa window are indistinguishable, so
	// the window is a light grey and the pressed state a step deeper.
	.screen_bg = LV_COLOR_MAKE(236, 237, 240),
	.surface = LV_COLOR_MAKE(255, 255, 255),
	.surface_pressed = LV_COLOR_MAKE(221, 223, 228),
	.panel = LV_COLOR_MAKE(255, 255, 255),
	.cover_bg = LV_COLOR_MAKE(224, 226, 230),
	.text_primary = LV_COLOR_MAKE(50, 50, 50),
	.text_secondary = LV_COLOR_MAKE(115, 115, 115),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = false,
};

// Whether white type stops reading on a fill. The lightest of the accent
// presets weighs about 183, so only a deliberately pale accent crosses it.
#define ON_ACCENT_DARK_INK_LUMA 200

bool theme_color_is_light(lv_color_t c) {
	return (299u * c.red + 587u * c.green + 114u * c.blue) / 1000u > ON_ACCENT_DARK_INK_LUMA;
}

// The selected preset is kept apart from `active` because dynamic tint points
// `active` at a derived copy rather than at either preset.
static theme_kind_t selected_kind = THEME_KIND_DARK;

// The layout style, chosen separately from the palette and defaulting to the
// V1's original interface.
static theme_style_t selected_style = THEME_STYLE_CLASSIC;

static theme_palette_t *selected_palette(void) {
	switch (selected_kind) {
	case THEME_KIND_LIGHT:
		return &palette_light;
	case THEME_KIND_DARK:
	default:
		return &palette_dark;
	}
}

// The dynamic tint: the neutral colours -- backgrounds, cards, the status bar
// -- borrow the accent's hue, the way Material You colours a system from one
// seed. Off by default, remembered in the config.
static bool dynamic_tint;

// The tinted copy of whichever preset is chosen. Rebuilt on every theme,
// accent or tint change; `active` points here while the tint is on.
static theme_palette_t tinted;

static theme_palette_t *active = &palette_dark;

// The accent presets, Adwaita's named accents plus four more to fill the
// gaps between them -- lime between the yellow and the green, indigo between
// the blue and the purple -- and the two neutrals, brown and slate, at the
// end. Blue (the default) comes first, then warm to cool: red, orange,
// yellow, lime, green, cyan, indigo, purple, pink.
static const lv_color_t ACCENT_COLORS[THEME_ACCENT_COUNT] = {
	LV_COLOR_MAKE(53, 132, 228),  // blue (#3584e4, the default)
	LV_COLOR_MAKE(224, 27, 36),   // red (#e01b24)
	LV_COLOR_MAKE(255, 120, 0),   // orange (#ff7800)
	LV_COLOR_MAKE(229, 165, 10),  // yellow (#e5a50a)
	LV_COLOR_MAKE(153, 197, 0),   // lime (#99c500)
	LV_COLOR_MAKE(46, 194, 126),  // green (#2ec27e)
	LV_COLOR_MAKE(0, 176, 214),   // cyan (#00b0d6)
	LV_COLOR_MAKE(102, 91, 224),  // indigo (#665be0)
	LV_COLOR_MAKE(145, 65, 172),  // purple (#9141ac)
	LV_COLOR_MAKE(213, 97, 153),  // pink (#d56199)
	LV_COLOR_MAKE(168, 101, 48),  // brown (#a86530)
	LV_COLOR_MAKE(139, 142, 148), // slate (#8b8e94)
};
static int accent_index;

static lv_color_t default_accent(theme_kind_t kind) {
	(void)kind;
	return ACCENT_COLORS[0];
}

// Where the six presets of the old palette sit in this one: a config written
// by an older firmware names an old slot, so the first boot on this build
// moves the choice to the matching new slot and leaves the marker that the
// init reads below, so the remap runs once rather than on every boot.
static const int LEGACY_ACCENT_MAP[6] = {0, 2, 3, 5, 8, 9};

lv_color_t theme_accent_preset(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	// Slot zero is the shared blue default; the other slots are the same in
	// both palettes.
	return index == 0 ? default_accent(selected_kind) : ACCENT_COLORS[index];
}

int theme_get_accent(void) { return accent_index; }

// Callers asking to be called back after a theme or accent change. The list
// grows on demand and has no fixed limit: a dropped registration shows up only
// as a page that keeps the colours it was built with, which is invisible until
// somebody changes the accent. A failed allocation is logged for the same
// reason.
static theme_refresh_cb_t *refresh_cbs;
static int refresh_cb_count;
static int refresh_cb_capacity;

// How much of the accent's own saturation a neutral borrows. Dark surfaces
// take more before the hue stops reading as a shade and starts reading as a
// colour; the light ones are nearly white, where the same amount would turn
// the page pastel.
#define TINT_STRENGTH_DARK 0.30f
#define TINT_STRENGTH_LIGHT 0.15f

// Gives a neutral the accent's hue while keeping exactly how light it is: the
// brightest channel of the result is the brightest channel of the input. The
// colour is never pulled towards the accent's own brightness, which is what
// would turn a dark background light under a pale accent; the two channels
// below the brightest do come down, so a tinted surface is fractionally darker
// than the plain one, and that is what the two strengths above are set from.
static lv_color_t tint_with(lv_color_t c, lv_color_t accent, float strength) {
	uint8_t amax = accent.red > accent.green ? accent.red : accent.green;
	if (accent.blue > amax) {
		amax = accent.blue;
	}
	uint8_t amin = accent.red < accent.green ? accent.red : accent.green;
	if (accent.blue < amin) {
		amin = accent.blue;
	}
	uint8_t cmax = c.red > c.green ? c.red : c.green;
	if (c.blue > cmax) {
		cmax = c.blue;
	}
	if (amax == amin || cmax == 0) {
		return c; // a grey accent has no hue to lend, and black takes none
	}

	// The accent's shape: 1 on its brightest channel, 0 on its darkest. Applied
	// to the neutral's own brightness it reproduces that hue at the neutral's
	// level, at a fraction of the accent's saturation.
	float span = (float)(amax - amin);
	float sat = span / (float)amax * strength;
	float shape[3] = {
		((float)accent.red - (float)amin) / span,
		((float)accent.green - (float)amin) / span,
		((float)accent.blue - (float)amin) / span,
	};

	uint8_t out[3];
	for (int i = 0; i < 3; i++) {
		out[i] = (uint8_t)((float)cmax * (1.0f - sat * (1.0f - shape[i])) + 0.5f);
	}
	return lv_color_make(out[0], out[1], out[2]);
}

// Points `active` at the chosen preset, or at a tinted copy of it.
static void rebuild_active(void) {
	theme_palette_t *base = selected_palette();

	if (!dynamic_tint) {
		active = base;
		return;
	}

	float strength = base->dark ? TINT_STRENGTH_DARK : TINT_STRENGTH_LIGHT;
	tinted = *base;
	tinted.screen_bg = tint_with(base->screen_bg, base->accent, strength);
	tinted.surface = tint_with(base->surface, base->accent, strength);
	tinted.surface_pressed = tint_with(base->surface_pressed, base->accent, strength);
	tinted.panel = tint_with(base->panel, base->accent, strength);
	tinted.cover_bg = tint_with(base->cover_bg, base->accent, strength);
	// Text keeps the plain colours: the monochrome icons are recoloured with
	// text_primary, and they are meant to come through the tint untouched.
	active = &tinted;
}

// Pushes the active palette into the shared styles.
static void apply_palette(void) {
	lv_style_set_bg_color(&theme_style_screen, active->screen_bg);

	lv_style_set_bg_color(&theme_style_panel, active->panel);

	lv_style_set_bg_color(&theme_style_card, active->surface);
	lv_style_set_text_color(&theme_style_card, active->text_primary);

	lv_style_set_bg_color(&theme_style_card_pressed, active->surface_pressed);

	lv_style_set_text_color(&theme_style_text, active->text_primary);
	lv_style_set_text_color(&theme_style_text_dim, active->text_secondary);

	lv_style_set_image_recolor(&theme_style_icon, active->text_primary);

	lv_style_set_bg_color(&theme_style_switch, active->surface_pressed);
	lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_WIDTH);
	lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_COLOR);
	lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_OPA);
	lv_style_remove_prop(&theme_style_switch_track_checked, LV_STYLE_BG_COLOR);
	lv_style_remove_prop(&theme_style_switch_track_checked, LV_STYLE_BORDER_COLOR);
	lv_style_remove_prop(&theme_style_switch_disabled, LV_STYLE_RECOLOR_OPA);
	lv_style_remove_prop(&theme_style_switch_disabled, LV_STYLE_OPA);
	lv_style_set_bg_color(&theme_style_switch_checked, active->accent);
	lv_style_set_bg_color(&theme_style_switch_knob_checked, theme_on_accent());

	lv_style_set_bg_color(&theme_style_accent_bg, active->accent);
	lv_style_set_text_color(&theme_style_accent_bg, theme_on_accent());
	lv_style_set_bg_color(&theme_style_slider_track, active->text_secondary);
}

// Which picture the next power-on shows (Appearance > Boot screen): the
// stock light/dark pair or Retrospace. Retrospace is the default, and it is
// also what a device with no marker at all draws, because the stock script's
// fallback file is the one that carries it -- see bootlogo.h.
int theme_boot_screen(void) {
	int choice = (int)config_get_int("ui", "boot_screen", BOOTLOGO_RETROSPACE);
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_RETROSPACE) {
		choice = BOOTLOGO_RETROSPACE; // a config written by a newer firmware
	}
	return choice;
}

// Remembers the choice and moves the flash marker now rather than at the next
// theme switch: the setting is only worth anything from the next boot on.
void theme_set_boot_screen(int choice) {
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_RETROSPACE) {
		return;
	}

	config_set_int("ui", "boot_screen", choice);
	config_save();

	bootlogo_set(choice, active->dark);
}

void theme_init(void) {
	lv_style_init(&theme_style_screen);
	lv_style_set_bg_opa(&theme_style_screen, LV_OPA_COVER);

	lv_style_init(&theme_style_panel);
	lv_style_set_bg_opa(&theme_style_panel, LV_OPA_COVER);

	lv_style_init(&theme_style_card);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);

	lv_style_init(&theme_style_card_pressed);
	lv_style_set_bg_opa(&theme_style_card_pressed, LV_OPA_COVER);
	// The default theme grows a pressed button by a few pixels; on a list of
	// cards that reads as the row jumping out of the list, so pin it flat.
	lv_style_set_transform_width(&theme_style_card_pressed, 0);
	lv_style_set_transform_height(&theme_style_card_pressed, 0);

	lv_style_init(&theme_style_text);
	lv_style_init(&theme_style_text_dim);

	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);

	lv_style_init(&theme_style_switch);
	lv_style_set_bg_opa(&theme_style_switch, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_checked);
	lv_style_set_bg_opa(&theme_style_switch_checked, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_knob_checked);
	lv_style_set_bg_opa(&theme_style_switch_knob_checked, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_track_checked);
	lv_style_init(&theme_style_switch_disabled);

	lv_style_init(&theme_style_accent_bg);
	lv_style_set_bg_opa(&theme_style_accent_bg, LV_OPA_COVER);
	lv_style_init(&theme_style_slider_track);
	lv_style_set_bg_opa(&theme_style_slider_track, LV_OPA_40);
	lv_style_set_radius(&theme_style_slider_track, LV_RADIUS_CIRCLE);

	// Read the named theme choice when present; configs from older builds have
	// only `dark_theme`, so they keep their original Dark/Light setting. A
	// palette this build no longer has (tokyo-night, amber-on-black, oled)
	// falls through to that same boolean, which is true by default.
	const char *saved_theme = config_get("ui", "theme", NULL);
	if (saved_theme && strcmp(saved_theme, "light") == 0) {
		selected_kind = THEME_KIND_LIGHT;
	} else if (saved_theme && strcmp(saved_theme, "dark") == 0) {
		selected_kind = THEME_KIND_DARK;
	} else {
		selected_kind = config_get_bool("ui", "dark_theme", true) ? THEME_KIND_DARK : THEME_KIND_LIGHT;
	}

	// The layout style. A clean boot gets the V1's original interface. A config
	// written before the styles existed kept its layout in the theme name: the
	// three palettes that used the compact one map to Morpthis, so an existing
	// device keeps the look it had.
	const char *saved_style = config_get("ui", "style", NULL);
	if (saved_style && (strcmp(saved_style, "morpthis") == 0 || strcmp(saved_style, "sonix") == 0)) {
		selected_style = THEME_STYLE_MORPTHIS;
	} else if (saved_style && strcmp(saved_style, "classic") == 0) {
		selected_style = THEME_STYLE_CLASSIC;
	} else if (saved_theme && (strcmp(saved_theme, "tokyo-night") == 0 ||
							  strcmp(saved_theme, "amber-on-black") == 0 || strcmp(saved_theme, "oled") == 0)) {
		selected_style = THEME_STYLE_MORPTHIS;
	}

	// The marker the boot script reads has to agree with the setting too, not
	// only with the theme.
	bootlogo_set(theme_boot_screen(), selected_palette()->dark);

	// The accent slot is shared across themes; slot zero resolves to each new
	// palette's native accent while the other slots retain their shared colors.
	accent_index = (int)config_get_int("ui", "accent", 0);
	if (accent_index < 0 || accent_index >= THEME_ACCENT_COUNT) {
		accent_index = 0;
	}
	if (config_get_int("ui", "accent_scheme", 0) == 0) {
		// No marker: the value came from the six-preset palette. Move the
		// choice to its place among the twelve and write the marker, so a
		// boot after this one reads the index as it is.
		if (accent_index < 6) {
			accent_index = LEGACY_ACCENT_MAP[accent_index];
		}
		config_set_int("ui", "accent", accent_index);
		config_set_int("ui", "accent_scheme", 1);
		config_save();
	}
	palette_dark.accent = ACCENT_COLORS[accent_index];
	palette_light.accent = ACCENT_COLORS[accent_index];

	dynamic_tint = config_get_bool("ui", "dynamic_tint", false);

	rebuild_active();
	apply_palette();
}

void theme_apply_slider_knob(lv_obj_t *slider) {
	if (!slider) {
		return;
	}
	bool dark = theme()->dark;
	// The white knob sits at the end of the accent fill. On a pale accent the
	// hairline is not enough to tell the two apart, so the knob gets a solid
	// black ring, which on a dark page reads as a gap.
	bool solid = dark && theme_color_is_light(theme()->accent);
	lv_obj_set_style_border_color(slider, dark ? lv_color_black() : lv_color_make(120, 120, 120), LV_PART_KNOB);
	lv_obj_set_style_border_opa(slider, dark && !solid ? LV_OPA_10 : LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(slider, dark && !solid ? 1 : 2, LV_PART_KNOB);
}

void theme_apply_switch(lv_obj_t *sw) {
	lv_obj_add_style(sw, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(sw, &theme_style_switch_track_checked, LV_PART_MAIN | LV_STATE_CHECKED);
	lv_obj_add_style(sw, &theme_style_switch_disabled, LV_PART_MAIN | LV_STATE_DISABLED);
	lv_obj_add_style(sw, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_style(sw, &theme_style_switch_knob_checked, LV_PART_KNOB | LV_STATE_CHECKED);
}

void theme_register_refresh(theme_refresh_cb_t cb) {
	if (!cb) {
		return;
	}

	if (refresh_cb_count == refresh_cb_capacity) {
		int wanted = refresh_cb_capacity ? refresh_cb_capacity * 2 : 16;
		theme_refresh_cb_t *grown = realloc(refresh_cbs, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			// A device that cannot find a few dozen bytes has bigger problems,
			// but a registration must never be lost unnoticed.
			fprintf(stderr, "theme: out of memory for refresh callback number %d\n",
					refresh_cb_count + 1);
			return;
		}
		refresh_cbs = grown;
		refresh_cb_capacity = wanted;
	}

	refresh_cbs[refresh_cb_count++] = cb;
}

const theme_palette_t *theme(void) { return active; }

lv_color_t theme_now_playing_fill(void) {
	return lv_color_mix(active->accent, active->surface, LV_OPA_10);
}

bool theme_is_dark(void) { return active->dark; }

// The three palettes these name were removed for the V1-only tree. The pages
// still ask, and take their ordinary Dark/Light drawing.
bool theme_is_tokyo_night(void) { return false; }

bool theme_is_amber_on_black(void) { return false; }

bool theme_is_oled(void) { return false; }

bool theme_is_monochrome(void) { return false; }

// The compact V1 layout and the function-matched glyphs belong to the Morpthis
// style alone; the palette has no say.
bool theme_uses_function_glyphs(void) { return selected_style == THEME_STYLE_MORPTHIS; }

theme_style_t theme_get_style(void) { return selected_style; }

lv_color_t theme_ink_on(lv_color_t fill) { return theme_color_is_light(fill) ? lv_color_black() : lv_color_white(); }

lv_color_t theme_on_accent(void) { return theme_ink_on(active->accent); }

theme_kind_t theme_get_kind(void) { return selected_kind; }

lv_color_t theme_semantic_color(theme_semantic_t role) {
	// Dark and Light resolve every destination glyph to the chosen accent;
	// muted labels keep the secondary text colour.
	return role == THEME_SEMANTIC_MUTED ? active->text_secondary : active->accent;
}

// The file browser's folders wear the semantic green, which resolves to the
// accent on both palettes.
//
// The names on the rows are not part of this: they read in the ordinary text
// colour, the same one the library lists write theirs in.
lv_color_t theme_browser_folder_color(void) {
	return theme_semantic_color(THEME_SEMANTIC_GREEN);
}

lv_color_t theme_player_chrome_color(theme_player_chrome_t role) {
	switch (role) {
	case THEME_PLAYER_CHROME_PRIMARY:
	case THEME_PLAYER_CHROME_CHEVRON:
		return active->text_primary;
	case THEME_PLAYER_CHROME_SECONDARY:
	case THEME_PLAYER_CHROME_MUTED:
	case THEME_PLAYER_CHROME_PROGRESS_REST:
	case THEME_PLAYER_CHROME_SHUFFLE_IDLE:
	case THEME_PLAYER_CHROME_OVERFLOW:
		return active->text_secondary;
	case THEME_PLAYER_CHROME_PLAYBACK:
	default:
		return active->accent;
	}
}

void theme_set_dark(bool dark) { theme_set_kind(dark ? THEME_KIND_DARK : THEME_KIND_LIGHT); }

// Which palette the pages on screen were last walked for.
//
// lv_obj_report_style_change(NULL) walks every object of every screen and
// refreshes each one against its whole subtree. The pages here are built at
// startup and stay built, so that is the entire interface -- thousands of
// objects -- for a change that moves nothing but colours, and it takes far
// longer than a frame: the finger is still on the switch when it starts.
//
// A colour is read out of the style at draw time, so the page in front of the
// user is the only one that has to be walked at the moment of the change. The
// pages behind it are walked the first time they are shown again, which is
// where a redraw was going to happen anyway.
static unsigned palette_generation = 1;

static struct screen_generation {
	lv_obj_t *screen;
	unsigned generation;
} *screen_gens;
static int screen_gen_count;
static int screen_gen_capacity;

// True when `screen` still carries an older palette than the one in force, and
// records it as walked for this one. A screen this has never seen is taken as
// current: it was built from the styles as they stand.
//
// An allocation that fails costs a walk per page change rather than a page
// with the wrong colours, which is why the failure is silent here.
static bool screen_is_stale(lv_obj_t *screen) {
	for (int i = 0; i < screen_gen_count; i++) {
		if (screen_gens[i].screen != screen) {
			continue;
		}
		if (screen_gens[i].generation == palette_generation) {
			return false;
		}
		screen_gens[i].generation = palette_generation;
		return true;
	}

	if (screen_gen_count == screen_gen_capacity) {
		int wanted = screen_gen_capacity ? screen_gen_capacity * 2 : 32;
		struct screen_generation *grown = realloc(screen_gens, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return true;
		}
		screen_gens = grown;
		screen_gen_capacity = wanted;
	}

	screen_gens[screen_gen_count].screen = screen;
	screen_gens[screen_gen_count].generation = palette_generation;
	screen_gen_count++;
	return false;
}

// One tree, refreshed against the styles it carries. LV_STYLE_PROP_ANY takes
// the object's own properties and recurses into its children.
static void refresh_tree(lv_obj_t *obj) {
	if (obj) {
		lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
	}
}

void theme_notify_screen_shown(lv_obj_t *screen) {
	if (screen && screen_is_stale(screen)) {
		refresh_tree(screen);
	}
}

// Repaints after a palette change: the page on screen and the three layers the
// status bar, the player sheet and the popups live on, then the callbacks,
// which catch what cannot be expressed as a shared style -- an icon recoloured
// at runtime, the blurred artwork behind the controls. Those run for every
// page, on screen or not, and cost about as much as one frame in total.
static void refresh_all(void) {
	palette_generation++;

	lv_obj_t *screen = lv_screen_active();
	screen_is_stale(screen);
	refresh_tree(screen);
	refresh_tree(lv_layer_top());
	refresh_tree(lv_layer_sys());
	refresh_tree(lv_layer_bottom());

	for (int i = 0; i < refresh_cb_count; i++) {
		refresh_cbs[i]();
	}
}

static const char *theme_config_value(theme_kind_t kind) {
	return kind == THEME_KIND_LIGHT ? "light" : "dark";
}

void theme_set_kind(theme_kind_t kind) {
	if (kind < THEME_KIND_DARK || kind >= THEME_KIND_COUNT || selected_kind == kind) {
		return;
	}

	selected_kind = kind;
	rebuild_active();
	apply_palette();

	// Keep the new string key and the old boolean in step: older builds still
	// read `dark_theme`.
	config_set("ui", "theme", theme_config_value(kind));
	config_set_bool("ui", "dark_theme", active->dark);
	config_save();

	// The boot script understands only light and dark.
	bootlogo_set(theme_boot_screen(), active->dark);
	refresh_all();
}

void theme_toggle(void) { theme_set_kind(active->dark ? THEME_KIND_LIGHT : THEME_KIND_DARK); }

void theme_set_style(theme_style_t style) {
	if (style < THEME_STYLE_CLASSIC || style >= THEME_STYLE_COUNT || selected_style == style) {
		return;
	}

	selected_style = style;
	config_set("ui", "style", style == THEME_STYLE_MORPTHIS ? "morpthis" : "classic");
	config_save();

	// The pages that lay themselves out differently listen through the refresh
	// callbacks registered with the theme; refresh_all() walks them.
	refresh_all();
}

void theme_set_accent(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	accent_index = index;
	palette_dark.accent = ACCENT_COLORS[index];
	palette_light.accent = ACCENT_COLORS[index];
	rebuild_active(); // the tint is drawn from the accent, so it moves with it
	apply_palette();

	config_set_int("ui", "accent", index);
	config_save();

	refresh_all();
}

bool theme_dynamic_tint(void) { return dynamic_tint; }

void theme_set_dynamic_tint(bool on) {
	if (dynamic_tint == on) {
		return;
	}
	dynamic_tint = on;
	rebuild_active();
	apply_palette();

	config_set_bool("ui", "dynamic_tint", on);
	config_save();

	refresh_all();
}
