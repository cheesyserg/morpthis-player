#ifndef FONTS_H
#define FONTS_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The UI fonts -- every size the interface draws with, and the face they are
// drawn from.
//
// There are no fonts inside the binary. Everything is rendered through
// FreeType from the faces shipped in /usr/resource/morpthis/fonts:
//
//   default.otf   MiSans Regular -- Latin, Greek, Cyrillic, kana, full CJK
//   bold.otf      optional bold weight for the headings; regular stands in
//                 when it is not there
//   korean.otf    Hangul, and the few Japanese marks MiSans lacks (the
//                 middle dot, the wave dash): a subset of Pretendard
//                 matched to MiSans
//   thai.otf      MiSans Thai
//   arabic.otf    MiSans Arabic
//   japanese.otf  FOT-Rodin, for kana, kanji and CJK punctuation when the
//                 interface is in Japanese
//
// MiSans is the face the interface was designed with, and the one it falls
// back to. Settable families are the others in the directory: Neon 80s on the
// V1, and Roboto, Roboto Condensed, Inter and Barlow Semi Condensed
// (their files are optional -- a family the firmware does not carry is simply
// not offered). MiSans stays at the end of every chain, so a face with no
// Cyrillic, no kanji or no Hangul still draws the whole interface.
//
// The script faces each have an optional -bold file, like bold.otf.
// fonts_init() opens them once and builds one fallback chain per size.
// In Japanese, Rodin answers the CJK letters ahead of default (see fonts.c).
// FreeType maps the files -- nothing is decoded up front -- and rendered
// glyphs live in the shared FTC cache (LV_FREETYPE_CACHE_FT_GLYPH_CNT), so
// memory stays flat no matter how much text is on screen.
//
// Arabic is drawn right to left and in its joined forms by LVGL itself
// (LV_USE_BIDI and LV_USE_ARABIC_PERSIAN_CHARS in lv_conf.h); the face only
// has to carry the presentation forms, which MiSans Arabic does.
//
// These are real objects, not pointers, so `&font_ui_24` is a valid
// lv_font_t * at every call site. A face change fills them in again, which is
// why nothing may hold on to a font they were copied from.
extern lv_font_t font_ui_14;
extern lv_font_t font_ui_16;
extern lv_font_t font_ui_18;
extern lv_font_t font_ui_20;
extern lv_font_t font_ui_20_bold; // bold text in the release notes of the update card
extern lv_font_t font_ui_22;
extern lv_font_t font_ui_22_bold; // headings in the release notes of the update card
extern lv_font_t font_ui_24;
extern lv_font_t font_ui_24_bold;
extern lv_font_t font_ui_26;
extern lv_font_t font_ui_28;
extern lv_font_t font_ui_32;
// The letter shown in the middle of the screen while the A-Z list index is in
// use: one letter at a time, and it has to read from a distance.
extern lv_font_t font_ui_64_bold;
extern lv_font_t font_ui_72; // the screensaver's clock, nothing else is this big

// Loads the faces and fills the objects above. Must run after lv_init() (it
// uses LVGL's FreeType binding) and before the first widget is created.
// Returns false when the default face could not be opened at all -- nothing
// can be drawn without it, so the caller should treat that as fatal.
bool fonts_init(void);

// Settings -> Appearance -> Text size, stored as [ui] text_size and read by
// fonts_init().
#define FONTS_TEXT_NORMAL 0
#define FONTS_TEXT_LARGE 1
#define FONTS_TEXT_SMALL 2 // keep existing saved values for normal and large
#define FONTS_TEXT_COUNT 3

// The active text size (see fonts.c).
int fonts_text_size(void);

// Switches the size on the running interface: the objects above are drawn at
// the new size from the next frame, and every page is measured and laid out
// again. Does not touch the config. False, changing nothing, when the faces
// cannot be opened at the new sizes or the value is invalid.
bool fonts_set_text_size(int size);

// Called after every such switch, for a page that worked something out from a
// font when it was built and has to work it out again.
void fonts_register_change(void (*cb)(void));

// ---------------------------------------------------------------------------
// Settings -> Appearance -> UI font
//
// Which face the whole interface is drawn with, stored as [ui] font. The
// value is one of the ids below, or "card:<file name>" for a font the user
// put in the Fonts folder on the card.
// ---------------------------------------------------------------------------

// The faces on offer, in the order the picker shows them: `ids` are what
// fonts_set_face() and the config take, `labels` are what the buttons say (a
// face's own name, the same in every language, so not put through tr()).
// Both arrays receive `max` entries at most; the count is what is returned.
//
// A family whose files are not in this firmware is left out, and so is every
// card font while there is no card. The list therefore only ever holds faces
// that can actually be opened.
int fonts_choices(const char **ids, const char **labels, int max);

// The face in force -- normally the stored choice, but the default face when
// the stored one could not be opened (a card font whose card has been taken
// out). Never NULL.
const char *fonts_choice(void);

// Switches the face on the running interface, the way fonts_set_text_size()
// switches the size: the objects above are refilled, every page is measured
// and laid out again, and the fonts the old face was drawing through are
// handed back. Does not touch the config.
//
// False, with nothing on screen touched, when the face cannot be opened -- a
// file that is not there, or one too large to hold in memory.
bool fonts_set_face(const char *id);

// Where the card's own fonts live: <root>/Fonts. Call once the card root is
// known and before the pages are built. A card font chosen before the player
// started is picked up here, and the picker's list of card fonts is read
// after this.
void fonts_set_card_root(const char *sd_root);

// The language file whose interface draws CJK letters from japanese.otf.
#define FONTS_JAPANESE_LANGUAGE "Japanese"

// The interface language is about to become `name` (a language file's name,
// as lang_set() takes it). Call before lang_set(), so the relabelled pages are
// measured with the right faces.
void fonts_set_language(const char *name);

// For the developer options page: which font files are in use, e.g.
// "default.otf, bold.otf, Korean + Thai + Arabic". Never NULL.
const char *fonts_summary(void);

#endif // FONTS_H
