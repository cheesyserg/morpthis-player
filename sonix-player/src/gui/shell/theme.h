#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The built-in appearances: Dark and Light.
typedef enum {
	THEME_KIND_DARK = 0,
	THEME_KIND_LIGHT = 1,
} theme_kind_t;

#define THEME_KIND_COUNT 2

// Semantic accents for the V1's function-matched glyphs. Dark and Light
// resolve every role to the chosen accent (or secondary text for muted), so
// the glyphs still carry the destination's meaning through the layout.
typedef enum {
	THEME_SEMANTIC_BLUE,
	THEME_SEMANTIC_CYAN,
	THEME_SEMANTIC_PURPLE,
	THEME_SEMANTIC_GREEN,
	THEME_SEMANTIC_AMBER,
	THEME_SEMANTIC_RED,
	THEME_SEMANTIC_MUTED,
} theme_semantic_t;

// Role-specific colors from the reference Now Playing screen. Dark and Light
// map these to their ordinary text/accent colours.
typedef enum {
	THEME_PLAYER_CHROME_PRIMARY,
	THEME_PLAYER_CHROME_SECONDARY,
	THEME_PLAYER_CHROME_MUTED,
	THEME_PLAYER_CHROME_PLAYBACK,
	THEME_PLAYER_CHROME_PROGRESS_REST,
	THEME_PLAYER_CHROME_CHEVRON,
	THEME_PLAYER_CHROME_SHUFFLE_IDLE,
	THEME_PLAYER_CHROME_OVERFLOW,
} theme_player_chrome_t;

// The handful of colours the whole UI is built from. Everything on screen
// pulls from the active one.
typedef struct {
	lv_color_t screen_bg;	   // page background
	lv_color_t surface;		   // list rows, cards
	lv_color_t surface_pressed; // the same, while a finger is on it
	lv_color_t panel;		   // status bar and the player's control block
	lv_color_t cover_bg;	   // behind the artwork, when a track has none
	lv_color_t text_primary;
	lv_color_t text_secondary;
	lv_color_t accent;		   // the selected theme's active colour
	bool dark;
} theme_palette_t;

// Shared styles. Widgets add these instead of setting colours themselves,
// which is what makes switching themes a one-liner: update the style, LVGL
// repaints everything using it.
extern lv_style_t theme_style_screen;
extern lv_style_t theme_style_panel;
extern lv_style_t theme_style_card;
extern lv_style_t theme_style_card_pressed;
extern lv_style_t theme_style_text;
extern lv_style_t theme_style_text_dim;
extern lv_style_t theme_style_icon;
extern lv_style_t theme_style_switch;
extern lv_style_t theme_style_switch_checked; // image_recolor for monochrome icons

// The knob of a switch that is on. It sits on the accent-coloured track, so it
// takes theme_on_accent().
// theme_apply_switch() adds it with LV_PART_KNOB | LV_STATE_CHECKED.
extern lv_style_t theme_style_switch_knob_checked;

// Two states of the track that no built-in theme draws differently; they stay
// empty and are kept for the switch helper's shape.
extern lv_style_t theme_style_switch_track_checked;
extern lv_style_t theme_style_switch_disabled;

// Gives an lv_switch all of the switch styles, each on its part and state.
// Call it once, when the switch is built.
void theme_apply_switch(lv_obj_t *sw);

// A plain accent-coloured fill. Anything that has to follow the accent colour
// without the page repainting it adds this instead of setting the colour by
// hand: the sliders' filled part, the pill buttons. A hand-set colour is a
// snapshot -- it keeps whichever accent was live when the page was built, and
// does not follow a later change. Its text colour is theme_on_accent(), which
// a label inside the filled widget inherits.
extern lv_style_t theme_style_accent_bg;

// The unfilled part of a slider's track: the secondary text colour at low
// opacity, which reads as a quiet groove on both palettes.
extern lv_style_t theme_style_slider_track;

// Anything that can't be expressed as a style -- an icon recoloured at runtime,
// the blurred artwork behind the controls -- registers a callback here and gets
// called after every switch.
typedef void (*theme_refresh_cb_t)(void);
void theme_register_refresh(theme_refresh_cb_t cb);

// The ring that tells a slider's knob from what is behind it. In the dark
// theme a hairline of black is plenty; in the light one the cards are nearly
// white too, so a white knob needs a real grey outline not to vanish into
// them. Call it when the slider is built and from the page's theme-refresh
// callback -- the colour depends on the theme in force.
void theme_apply_slider_knob(lv_obj_t *slider);

// Builds the shared styles. Call once, before any screen is created.
void theme_init(void);

// Call with the page about to be put on screen. A theme, accent or tint change
// walks only the page the user is looking at; this is what walks a page that
// was hidden when the palette moved, and does nothing when it was not.
void theme_notify_screen_shown(lv_obj_t *screen);

const theme_palette_t *theme(void);
// The subtle accent-tinted surface shared by the dark themes' mini-player
// and Music browse tiles.
lv_color_t theme_now_playing_fill(void);
bool theme_is_dark(void);
// Absent from this tree. Kept as predicates because the pages still ask --
// their branches resolve to the ordinary Dark/Light drawing, which is what a
// V1 shows now.
bool theme_is_tokyo_night(void);
bool theme_is_amber_on_black(void);
bool theme_is_oled(void);
bool theme_is_monochrome(void);
// True: the function-matched icons and the compact V1 layouts are the only
// ones this tree draws. They used to be tied to the removed palettes.
bool theme_uses_function_glyphs(void);
theme_kind_t theme_get_kind(void);

// Dark and Light resolve every icon role to the chosen accent, with muted
// left to secondary text.
lv_color_t theme_semantic_color(theme_semantic_t role);
lv_color_t theme_player_chrome_color(theme_player_chrome_t role);

// What text and marks drawn on an accent-coloured fill wear: white, unless the
// accent is so light that white would not read on it.
lv_color_t theme_on_accent(void);
// The same rule for any other fill: white, or black on a fill that light.
lv_color_t theme_ink_on(lv_color_t fill);
// Whether a colour is that light: white type would not read on it.
bool theme_color_is_light(lv_color_t c);

// What the file browser recolours its folder glyphs to, and nothing else on
// the page: the semantic green, which is the accent.
//
// Read at paint time: a hand-set colour is a snapshot, so the page asks again
// whenever the palette moves.
lv_color_t theme_browser_folder_color(void);

// Switches between Dark and Light, and repaints the UI.
void theme_toggle(void);

// The accent colour, chosen from a fixed set of presets (Appearance > Accent
// colour): 0 blue (default), 1 red, 2 orange, 3 yellow, 4 lime, 5 green,
// 6 cyan, 7 indigo, 8 purple, 9 pink, 10 brown, 11 slate. Dark and Light
// share the same presets. Persists in the config.
#define THEME_ACCENT_COUNT 12
lv_color_t theme_accent_preset(int index);
int theme_get_accent(void);
void theme_set_accent(int index);

// Picks the legacy Dark or Light preset rather than flipping. Asking for Dark
// while any added palette is selected switches back to the original Dark
// palette rather than treating the dark palette as Dark.
void theme_set_dark(bool dark);

// Picks a specific built-in theme, saves it, and repaints the visible UI.
void theme_set_kind(theme_kind_t kind);

// The picture the next power-on shows (Appearance > Boot screen):
// BOOTLOGO_STOCK, the firmware's own light/dark pair, or one of the three
// added pictures -- BOOTLOGO_RETROSPACE, BOOTLOGO_SPACE, BOOTLOGO_TRAVELLING
// -- one image each for both palettes. The choice is kept in the config like
// every other setting, and pushed to the flash marker the boot script reads
// -- see bootlogo.h for why it cannot be a file. Retrospace is the default.
int theme_boot_screen(void);
void theme_set_boot_screen(int choice);

// The dynamic tint (Appearance > Dynamic tint): the neutral colours --
// backgrounds, cards, the status bar -- borrow the accent's hue, the way
// Material You colours a system from one seed. Each colour keeps its brightest
// channel, so a tinted surface stays at the level the preset put it at instead
// of drifting towards the accent's own; text and the monochrome icons are left
// alone entirely. Off by default, persists in the config.
bool theme_dynamic_tint(void);
void theme_set_dynamic_tint(bool on);

#endif // THEME_H
