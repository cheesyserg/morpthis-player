#include "fonts.h"

#include "src/system/core/respath.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"

#include "src/gui/board_profile.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// All text is drawn through LVGL's FreeType binding, from the faces the
// firmware ships in FONT_DIR. One FT_Face per file serves however many sizes
// use it; glyphs are rasterised on demand into a shared LRU cache.
//
// The lv_font_t objects the interface refers to (&font_ui_24 and friends) are
// statically allocated here and filled in by copying the fonts FreeType
// creates. The copy is safe because LVGL's FreeType callbacks reach their
// state only through font->dsc, which the copy carries along.
//
// Which face they are filled from is a setting (Settings -> Appearance -> UI
// font; see the table below). Every font FreeType creates for a face is
// recorded as it is made, so the whole face -- the file itself, the script
// faces behind it and MiSans at the end -- can be handed back and another
// built in its place. The face being switched to is built beside the one on
// screen and only copied in once it is complete, so a face that will not open
// leaves the interface as it is.

#define FONT_DIR MORPTHIS_RESOURCE_DIR "/fonts"

// Host fallback: the copies in the repository, when there is no resource tree.
#ifdef HOST_BUILD
#define FONT_DIR_FALLBACK "assets/fonts"
#else
#define FONT_DIR_FALLBACK FONT_DIR
#endif

// The main faces ship as .ttf on some firmwares and .otf on others, and
// FreeType reads both. Each face is therefore a list of candidate names, tried
// in order, and the first one present wins.
#define FACE_FILES(base)                                                                                               \
	{FONT_DIR "/" base ".ttf", FONT_DIR "/" base ".otf", FONT_DIR_FALLBACK "/" base ".ttf",                          \
	 FONT_DIR_FALLBACK "/" base ".otf", NULL}

static const char *const FONT_DEFAULT_FILES[] = FACE_FILES("default");
static const char *const FONT_BOLD_FILES[] = FACE_FILES("bold");
static const char *const FONT_NEON_FILES[] = FACE_FILES("Neon");
static const char *const FONT_ROBOTO_FILES[] = FACE_FILES("Roboto");
static const char *const FONT_ROBOTO_CONDENSED_FILES[] = FACE_FILES("RobotoCondensed");
static const char *const FONT_INTER_FILES[] = FACE_FILES("Inter");
static const char *const FONT_BARLOW_FILES[] = FACE_FILES("BarlowSemiCondensed");

// ---------------------------------------------------------------------------
// Settings -> Appearance -> UI font
//
// One entry per family the picker can offer, in the order it shows them.
// `label` is the family's own name: the same in every language, so it is not
// put through tr(). A family with a bold companion names it; without one the
// regular file answers the bold sizes as well, which is how the V1 has always
// drawn Neon 80s.
//
// A family whose files are not in this firmware is not offered at all -- the
// picker only lists what first_present() can find -- so the list never holds a
// face that cannot be opened.
// ---------------------------------------------------------------------------
typedef struct {
	const char *id; // what [ui] font stores, and what fonts_set_face() takes
	const char *label;
	const char *const *regular_files;
	const char *const *bold_files;
} ui_face_t;

#define FACE_DEFAULT_ID "default"

static const ui_face_t faces[] = {
	{FACE_DEFAULT_ID, "MiSans", FONT_DEFAULT_FILES, FONT_BOLD_FILES},
	{"neon", "Neon 80s", FONT_NEON_FILES, NULL},
	{"roboto", "Roboto", FONT_ROBOTO_FILES, NULL},
	{"roboto_condensed", "Roboto Condensed", FONT_ROBOTO_CONDENSED_FILES, NULL},
	{"inter", "Inter", FONT_INTER_FILES, NULL},
	{"barlow_semi_condensed", "Barlow Semi Condensed", FONT_BARLOW_FILES, NULL},
};

#define FACE_COUNT (sizeof(faces) / sizeof(faces[0]))

// ---------------------------------------------------------------------------
// The card's own fonts
//
// <card>/Fonts/*.ttf and *.otf are offered under the built-in families, by
// file name. They are read into the tmpfs before FreeType is allowed near
// them: the file is on removable media, and a glyph read out of a card that
// has just been pulled out is a SIGBUS, not an error code. The copy is named
// after the file it came from, so a face built from a card font and the face
// built from the last one are never the same path as far as FreeType's cache
// is concerned.
//
// One copy per font in use; it is removed as soon as the font is left behind.
// ---------------------------------------------------------------------------
#define CARD_DIR "Fonts"
#define CARD_COPY_PREFIX "/tmp/morpthis-font-"
#define CARD_NAME_MAX 96
#define CARD_LABEL_MAX 24 // what a pill can say before it is cut short
#define CARD_LIST_MAX 16                   // how many the picker lists
#define CARD_BYTES_MAX (4 * 1024 * 1024)   // more than this is not a UI font, and has to fit in RAM
#define CARD_ID_PREFIX "card:"

static char card_root[256];
static char card_ids[CARD_LIST_MAX][6 + CARD_NAME_MAX];  // "card:<name>"
static char card_labels[CARD_LIST_MAX][CARD_NAME_MAX];   // the same, without the extension
static int card_count;

// The scripts MiSans has no letters for, one face each, chained behind the
// main face so they only answer for what it lacks. The bold file is optional:
// the regular one stands in at the bold sizes when it is not there.
typedef struct {
	const char *name; // for the summary
	const char *const regular_files[5];
	const char *const bold_files[5];
	// Letters drawn this much higher than the file places them, in
	// thousandths of the size. See ARABIC_RAISE.
	int raise_per_mille;
	const char *regular; // the files found, or NULL
	const char *bold;
	bool used;
} script_face_t;

// MiSans Arabic sits lower than MiSans. Its tails, and the dots under them,
// reach 0.39 em below the baseline, where a line here has room for 0.28 (the
// main face's descent), and a label cuts off whatever passes its bottom edge:
// the two dots of a final ya were not drawn at all. Raised by this much, the
// deepest of the common letters ends where the line does.
#define ARABIC_RAISE 105

// Rodin, for Japanese. Chained right behind the main face and silent unless
// the interface is in Japanese; see japanese_ui below.
static script_face_t japanese = {"Japanese", FACE_FILES("japanese"), FACE_FILES("japanese-bold"), 0, NULL, NULL,
		false};

static script_face_t scripts[] = {
	{"Korean", FACE_FILES("korean"), FACE_FILES("korean-bold"), 0, NULL, NULL, false},
	{"Thai", FACE_FILES("thai"), FACE_FILES("thai-bold"), 0, NULL, NULL, false},
	{"Arabic", FACE_FILES("arabic"), FACE_FILES("arabic-bold"), ARABIC_RAISE, NULL, NULL, false},
};

#define SCRIPT_COUNT (sizeof(scripts) / sizeof(scripts[0]))

static const char *basename_of(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

// The one that exists, or NULL.
static const char *first_present(const char *const *candidates) {
	for (int i = 0; candidates[i]; i++) {
		if (access(candidates[i], R_OK) == 0) {
			return candidates[i];
		}
	}
	return NULL;
}

lv_font_t font_ui_14;
lv_font_t font_ui_16;
lv_font_t font_ui_18;
lv_font_t font_ui_20;
lv_font_t font_ui_20_bold;
lv_font_t font_ui_22;
lv_font_t font_ui_22_bold;
lv_font_t font_ui_24;
lv_font_t font_ui_24_bold;
lv_font_t font_ui_26;
lv_font_t font_ui_28;
lv_font_t font_ui_32;
lv_font_t font_ui_64_bold;
lv_font_t font_ui_72;

typedef struct {
	lv_font_t *font;
	int size;
	bool bold;
} ui_font_t;

static const ui_font_t ui_fonts[] = {
	{&font_ui_14, 14, false}, {&font_ui_16, 16, false}, {&font_ui_18, 18, false},
	{&font_ui_20, 20, false}, {&font_ui_22, 22, false}, {&font_ui_24, 24, false},
	{&font_ui_26, 26, false}, {&font_ui_28, 28, false}, {&font_ui_32, 32, false},
	{&font_ui_72, 72, false}, {&font_ui_20_bold, 20, true}, {&font_ui_22_bold, 22, true}, {&font_ui_24_bold, 24, true},
	{&font_ui_64_bold, 64, true},
};

#define UI_FONT_COUNT (sizeof(ui_fonts) / sizeof(ui_fonts[0]))

static char summary[160] = "";

// Settings -> Appearance -> Text size: Large. The small sizes -- the ones
// secondary lines, notes, clocks and counters are set in -- are drawn a step
// or more larger; headings, big numbers and the screensaver's clock stay as
// they are, since those were never hard to read. The objects keep their
// names: font_ui_22 is whatever 22 is drawn at, so every page follows without
// being told.
typedef struct {
	int size;
	int drawn;
} text_step_t;

static const text_step_t LARGE_STEPS[] = {
	{14, 17}, {16, 19}, {18, 21}, {20, 23}, {22, 25}, {24, 26},
};

// Small steps down the same secondary sizes; headings and clocks stay put.
static const text_step_t SMALL_STEPS[] = {
	{14, 12}, {16, 14}, {18, 16}, {20, 18}, {22, 20}, {24, 22},
};

static int text_size = FONTS_TEXT_NORMAL;

// The named font objects are used throughout the interface.  Remapping them
// here is both safer and more complete than chasing hundreds of explicit
// &font_ui_24 references: every page keeps its typography hierarchy, while a
// "24" font no longer consumes a tenth of the V1's entire width per word.
static int tempotec_drawn_size(int size, int setting) {
	int compact;
	switch (size) {
	case 14: compact = 14; break;
	case 16: compact = 15; break;
	case 18: compact = 15; break;
	case 20: compact = 16; break;
	case 22: compact = 16; break;
	case 24: compact = 17; break;
	case 26: compact = 18; break;
	case 28: compact = 19; break;
	case 32: compact = 21; break;
	case 36: compact = 23; break;
	case 64: compact = 32; break;
	case 72: compact = 36; break;
	default: compact = size; break;
	}
	// On the small panel one pixel each way makes a visible difference without
	// upsetting the compact layout. Leave the oversized letters and clock alone.
	if (size <= 36) {
		compact += setting == FONTS_TEXT_LARGE ? 1 : setting == FONTS_TEXT_SMALL ? -1 : 0;
	}
	return compact;
}

static int drawn_size(int size, int setting) {
	if (bp_is_tempotec_v1()) {
		return tempotec_drawn_size(size, setting);
	}
	const text_step_t *steps = setting == FONTS_TEXT_LARGE ? LARGE_STEPS : SMALL_STEPS;
	if (setting == FONTS_TEXT_NORMAL) {
		return size;
	}
	for (size_t i = 0; i < sizeof(LARGE_STEPS) / sizeof(LARGE_STEPS[0]); i++) {
		if (steps[i].size == size) {
			return steps[i].drawn;
		}
	}
	return size;
}

// ---------------------------------------------------------------------------
// The face in force, and the one being built in its place
//
// open_chain() reads the face it is building through `building`: the one in
// force while a size is being added, the one being switched to while a face is
// being changed.
// ---------------------------------------------------------------------------
typedef struct {
	char id[6 + CARD_NAME_MAX]; // what [ui] font stores
	const char *regular_file;   // as first_present() found it
	const char *bold_file;      // its bold companion, or NULL
	const char *fallback_regular; // MiSans behind it, when the face is not MiSans itself
	const char *fallback_bold;
	char card_copy[64 + CARD_NAME_MAX]; // the tmpfs copy, when the face came off the card
	bool misans_tail; // MiSans has to close every chain
} face_t;

static face_t active;
static face_t pending;
static const face_t *building = &active;

int fonts_text_size(void) { return text_size; }

// The extension that makes a file a face, and its length (0 for anything
// else). FreeType reads both containers by itself.
static size_t face_extension(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) {
		return 0;
	}
	static const char *const extensions[] = {".ttf", ".otf", ".ttc"};
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		if (strcasecmp(dot, extensions[i]) == 0) {
			return strlen(dot);
		}
	}
	return 0;
}

// `a` and `b` joined into `out`. A name that does not fit is left empty rather
// than cut short: half a file name reaches no font, and a reader can see that
// the size was checked here.
static bool join_path(char *out, size_t out_size, const char *a, const char *b) {
	size_t len_a = strlen(a);
	size_t len_b = strlen(b);
	if (len_a + len_b + 1 > out_size) {
		out[0] = 0;
		return false;
	}
	memcpy(out, a, len_a);
	memcpy(out + len_a, b, len_b + 1);
	return true;
}

// A name that is one file in the card's font folder, and nothing else.
static bool card_name_ok(const char *name) {
	if (!name || !name[0] || strlen(name) >= CARD_NAME_MAX) {
		return false;
	}
	if (strchr(name, '/') || name[0] == '.') {
		return false;
	}
	return face_extension(name) != 0;
}

// Reads <card>/Fonts/<name> into the tmpfs and reports where it landed.
// False when there is no card, no such file, or one too large to hold in RAM.
static bool card_font_stage(const char *name, char *out, size_t out_size) {
	if (!card_root[0] || !card_name_ok(name)) {
		return false;
	}

	char dir[sizeof(card_root) + sizeof(CARD_DIR) + 2];
	char source[sizeof(dir) + CARD_NAME_MAX];
	snprintf(dir, sizeof(dir), "%s/" CARD_DIR "/", card_root);
	if (!join_path(source, sizeof(source), dir, name)) {
		return false;
	}

	FILE *in = fopen(source, "rb");
	if (!in) {
		return false;
	}
	if (fseek(in, 0, SEEK_END) != 0) {
		fclose(in);
		return false;
	}
	long size = ftell(in);
	if (size <= 0 || size > CARD_BYTES_MAX || fseek(in, 0, SEEK_SET) != 0) {
		fclose(in);
		fprintf(stderr, "fonts: %s is %ld bytes, more than a UI font should be\n", name, size);
		return false;
	}

	char target[sizeof(CARD_COPY_PREFIX) + CARD_NAME_MAX];
	snprintf(target, sizeof(target), CARD_COPY_PREFIX "%s", name);

	FILE *copy = fopen(target, "wb");
	if (!copy) {
		fclose(in);
		return false;
	}
	char buffer[8192];
	size_t got;
	bool ok = true;
	while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (fwrite(buffer, 1, got, copy) != got) {
			ok = false;
			break;
		}
	}
	if (ferror(in)) {
		ok = false;
	}
	fclose(in);
	ok = fclose(copy) == 0 && ok;
	if (!ok) {
		remove(target);
		return false;
	}

	snprintf(out, out_size, "%s", target);
	return true;
}

// What a [ui] font value names, resolved to files. False when it names nothing
// this firmware has: an unknown id, or a card font that is not there any more.
//
// With `stage` set, a card font is read into the tmpfs as a side effect, and
// the caller owns the copy: one that goes on to fail has it to remove (see
// face_discard_copy()). Without it nothing is opened or copied -- which is what
// asking "is this the face already on screen?" needs, since a card font that is
// already being drawn with must not have the file under it rewritten.
static bool face_resolve(const char *id, face_t *out, bool stage) {
	if (!id || !id[0]) {
		id = FACE_DEFAULT_ID;
	}
	memset(out, 0, sizeof(*out));

	if (strncmp(id, CARD_ID_PREFIX, sizeof(CARD_ID_PREFIX) - 1) == 0) {
		snprintf(out->id, sizeof(out->id), "%s", id);
		if (!stage) {
			return card_name_ok(id + sizeof(CARD_ID_PREFIX) - 1);
		}
		const char *name = id + sizeof(CARD_ID_PREFIX) - 1;
		if (!card_font_stage(name, out->card_copy, sizeof(out->card_copy))) {
			return false;
		}
		out->regular_file = out->card_copy;
		out->fallback_regular = first_present(FONT_DEFAULT_FILES);
		out->fallback_bold = first_present(FONT_BOLD_FILES);
		out->misans_tail = true;
		return true;
	}

	for (size_t i = 0; i < FACE_COUNT; i++) {
		if (strcmp(id, faces[i].id) != 0) {
			continue;
		}
		const char *regular = first_present(faces[i].regular_files);
		if (!regular) {
			return false;
		}
		snprintf(out->id, sizeof(out->id), "%s", faces[i].id);
		out->regular_file = regular;
		out->bold_file = faces[i].bold_files ? first_present(faces[i].bold_files) : NULL;
		if (i != 0) {
			// Behind the chosen family: MiSans, for the letters it has and
			// this one has not. The default face is MiSans, so it needs no
			// second copy of itself.
			out->fallback_regular = first_present(FONT_DEFAULT_FILES);
			out->fallback_bold = first_present(FONT_BOLD_FILES);
			out->misans_tail = true;
		}
		return true;
	}
	return false;
}

// The RAM copy a face made for itself, once it is certain that face is not
// going to be used: it is nobody's otherwise, and the tmpfs is memory.
static void face_discard_copy(const face_t *face) {
	if (face->card_copy[0]) {
		remove(face->card_copy);
	}
}

// The card's fonts, by name, sorted so that adding one does not shuffle the
// ones already there. Re-read on every call: the picker asks once per visit,
// which is what lets a font copied over the USB cable appear without a reboot.
static void card_scan(void) {
	card_count = 0;
	if (!card_root[0]) {
		return;
	}

	char dir[sizeof(card_root) + sizeof(CARD_DIR) + 2];
	snprintf(dir, sizeof(dir), "%s/" CARD_DIR "/", card_root);
	DIR *handle = opendir(dir);
	if (!handle) {
		return;
	}

	int seen = 0;
	struct dirent *entry;
	while ((entry = readdir(handle)) != NULL) {
		if (!card_name_ok(entry->d_name)) {
			continue;
		}
		// A name that ends in .ttf is not enough: a directory can be called
		// one (the reader then fails on it), and on a card nothing says what a
		// file really is. Only regular files are offered.
		char path[sizeof(dir) + CARD_NAME_MAX];
		if (!join_path(path, sizeof(path), dir, entry->d_name)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		seen++;
		if (card_count >= CARD_LIST_MAX) {
			continue; // there to be counted, not to be listed
		}
		if (!join_path(card_ids[card_count], sizeof(card_ids[0]), CARD_ID_PREFIX, entry->d_name)) {
			continue;
		}
		// The pill says the file's name without its extension, cut short if
		// the name is a sentence: a label on a 240-pixel panel has no room
		// for one, and the pill would hang out of its card.
		size_t extension = face_extension(entry->d_name);
		size_t name_len = strlen(entry->d_name) - extension;
		if (name_len <= CARD_LABEL_MAX) {
			snprintf(card_labels[card_count], sizeof(card_labels[0]), "%.*s", (int)name_len, entry->d_name);
		} else {
			snprintf(card_labels[card_count], sizeof(card_labels[0]), "%.*s…", (int)(CARD_LABEL_MAX - 1),
					 entry->d_name);
		}
		card_count++;
	}
	closedir(handle);

	// Insertion sort: a handful of names at most, and no qsort comparator
	// casting to write.
	for (int i = 1; i < card_count; i++) {
		char id[sizeof(card_ids[0])];
		char label[sizeof(card_labels[0])];
		memcpy(id, card_ids[i], sizeof(id));
		memcpy(label, card_labels[i], sizeof(label));
		int j = i - 1;
		while (j >= 0 && strcasecmp(card_labels[j], label) > 0) {
			memcpy(card_ids[j + 1], card_ids[j], sizeof(card_ids[0]));
			memcpy(card_labels[j + 1], card_labels[j], sizeof(card_labels[0]));
			j--;
		}
		memcpy(card_ids[j + 1], id, sizeof(id));
		memcpy(card_labels[j + 1], label, sizeof(label));
	}

	if (seen > card_count) {
		fprintf(stderr, "fonts: %d fonts on the card, listing the first %d\n", seen, card_count);
	}
}

static const char *face_last_file(const char *path) {
	return path ? basename_of(path) : "";
}

// Everything needed to draw one size of one face, and everything that has to
// be handed back when it is replaced.
//
// `fonts` is what the named objects are copied from. `heap` is every lv_font_t
// FreeType created for that size -- the file itself and each face chained
// behind it -- which is what lv_freetype_font_delete() gives back. A copy
// never owns anything: the heap objects are the descriptors behind it.
#define FONT_CHAIN_MAX 12

typedef struct {
	lv_font_t fonts[UI_FONT_COUNT];
	lv_font_t *heap[UI_FONT_COUNT][FONT_CHAIN_MAX];
	uint8_t heap_len[UI_FONT_COUNT];
	bool built;
} font_set_t;

// One lazily built set per text-size setting, all belonging to the active face.
static font_set_t current_sets[FONTS_TEXT_COUNT];

// The size being built while a face is switched. A whole set rather than a
// pointer into the ones above, so that the face on screen is still complete
// and drawable if the new one will not open.
static font_set_t staged_set;

// The face name for the log and the developer options page: the files in use,
// and the scripts chained behind them.
static void build_summary(void);

static lv_font_t *open_face(const char *path, int size) {
	if (!path || access(path, R_OK) != 0) {
		return NULL;
	}
	return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
								   LV_FREETYPE_FONT_STYLE_NORMAL);
}

// LVGL's FreeType lookup, which every face starts with. A raised face goes
// through it and then moves the glyph up by the pixels kept in its user_data.
static bool (*freetype_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);

static bool raised_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter,
							 uint32_t letter_next) {
	if (!freetype_glyph_dsc(font, dsc, letter, letter_next)) {
		return false;
	}
	dsc->ofs_y += (int32_t)(intptr_t)font->user_data;
	return true;
}

// With the interface in Japanese, kana, kanji and the CJK punctuation and
// full-width forms are drawn from Rodin rather than MiSans, whose kanji are
// the Chinese shapes. The main face then passes on those letters, Rodin
// answers them, and a copy of the main face at the end of the chain covers the
// kanji Rodin does not have. In any other language Rodin passes on everything
// and the chain behaves as if it were not there.
static bool japanese_ui;

static bool is_cjk(uint32_t letter) {
	return (letter >= 0x3000 && letter <= 0x33FF) || // punctuation, kana, enclosed and compatibility forms
		   (letter >= 0x3400 && letter <= 0x4DBF) || // extension A
		   (letter >= 0x4E00 && letter <= 0x9FFF) || // unified ideographs
		   (letter >= 0xF900 && letter <= 0xFAFF) || // compatibility ideographs
		   (letter >= 0xFF00 && letter <= 0xFFEF);	 // full-width and half-width forms
}

static bool main_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t letter_next) {
	if (japanese_ui && is_cjk(letter)) {
		return false;
	}
	return freetype_glyph_dsc(font, dsc, letter, letter_next);
}

static bool japanese_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter,
							   uint32_t letter_next) {
	if (!japanese_ui) {
		return false;
	}
	return freetype_glyph_dsc(font, dsc, letter, letter_next);
}

static void raise_face(lv_font_t *face, int size, int per_mille) {
	int pixels = (size * per_mille + 500) / 1000;
	if (pixels <= 0) {
		return;
	}
	freetype_glyph_dsc = face->get_glyph_dsc;
	face->user_data = (void *)(intptr_t)pixels;
	face->get_glyph_dsc = raised_glyph_dsc;
}

// One face opened for one size, and remembered: what FreeType creates for a
// set is what that set has to give back, and nothing else knows the whole
// chain.
static lv_font_t *chain_open(font_set_t *set, size_t index, const char *path, int size) {
	lv_font_t *font = open_face(path, size);
	if (!font) {
		return NULL;
	}
	if (set->heap_len[index] >= FONT_CHAIN_MAX) {
		// The chains built here are shorter than the list; one that is not
		// would leave fonts behind that nothing can give back, so it is
		// refused instead.
		fprintf(stderr, "fonts: chain too long for the %d slots kept at %dpx\n", (int)FONT_CHAIN_MAX, size);
		lv_freetype_font_delete(font);
		return NULL;
	}
	set->heap[index][set->heap_len[index]++] = font;
	return font;
}

// One size as the interface draws it: the main face with the other scripts
// chained behind. False when the main face cannot be opened.
static bool open_chain(const ui_font_t *ui, int size, font_set_t *set, size_t index) {
	// The face this size draws from. The bold heading font uses the bold
	// file when the firmware has one; the regular stands in otherwise,
	// which reads fine at heading sizes even if it is not actually heavier.
	lv_font_t *head = NULL;
	if (ui->bold && building->bold_file) {
		head = chain_open(set, index, building->bold_file, size);
	}
	if (!head) {
		head = chain_open(set, index, building->regular_file, size);
	}
	if (!head) {
		fprintf(stderr, "fonts: cannot open %s at %dpx\n", building->regular_file, size);
		return false;
	}

	freetype_glyph_dsc = head->get_glyph_dsc;
	lv_font_t *tail = head;

	lv_font_t *rodin = NULL;
	if (ui->bold && japanese.bold) {
		rodin = chain_open(set, index, japanese.bold, size);
	}
	if (!rodin && japanese.regular) {
		rodin = chain_open(set, index, japanese.regular, size);
	}
	if (rodin) {
		head->get_glyph_dsc = main_glyph_dsc;
		rodin->get_glyph_dsc = japanese_glyph_dsc;
		tail->fallback = rodin;
		tail = rodin;
		japanese.used = true;
	}

	for (size_t i = 0; i < SCRIPT_COUNT; i++) {
		script_face_t *script = &scripts[i];
		lv_font_t *face = NULL;
		if (ui->bold && script->bold) {
			face = chain_open(set, index, script->bold, size);
		}
		if (!face && script->regular) {
			face = chain_open(set, index, script->regular, size);
		}
		if (face) {
			if (script->raise_per_mille) {
				raise_face(face, size, script->raise_per_mille);
			}
			tail->fallback = face;
			tail = face;
			script->used = true;
		}
	}

	// A family other than MiSans carries only the letters it carries: keep
	// MiSans at the end of the chain for everything else -- the Cyrillic, the
	// Hangul, the kanji, the symbols -- so the whole interface still draws.
	// Bold labels try MiSans Bold first, then regular MiSans as a final
	// safety net for any remaining glyphs.
	if (building->fallback_regular) {
		const char *fallback_path =
			(ui->bold && building->fallback_bold) ? building->fallback_bold : building->fallback_regular;
		lv_font_t *fallback = chain_open(set, index, fallback_path, size);
		if (fallback) {
			tail->fallback = fallback;
			tail = fallback;
		}
		if (ui->bold && building->fallback_bold && building->fallback_regular) {
			fallback = chain_open(set, index, building->fallback_regular, size);
			if (fallback) {
				tail->fallback = fallback;
				tail = fallback;
			}
		}
	}

	// In the MiSans-first builds, the main face again answers anything Rodin
	// lacks for Japanese. The MiSans tail above does that for every other
	// face, and is already in the chain by this point.
	if (rodin && !building->misans_tail) {
		lv_font_t *again = NULL;
		if (ui->bold && building->bold_file) {
			again = chain_open(set, index, building->bold_file, size);
		}
		if (!again) {
			again = chain_open(set, index, building->regular_file, size);
		}
		if (again) {
			tail->fallback = again;
		}
	}

	// The copy the interface draws through, with the chain behind it.
	set->fonts[index] = *head;
	return true;
}

// Gives back everything a set made: one lv_freetype_font_delete() per
// lv_freetype_font_create(), which is what drops the FT_Face behind them and
// the glyphs cached for it. Entries borrowed from the other size were never
// this set's to free, and have no heap list of their own.
static void free_font_set(font_set_t *set) {
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		for (uint8_t j = 0; j < set->heap_len[i]; j++) {
			lv_freetype_font_delete(set->heap[i][j]);
		}
		set->heap_len[i] = 0;
	}
	set->built = false;
}

// Fills `set` with the requested size of the face `building` describes.
// Sizes drawn at the same pixels can borrow fonts from an already-built set
// of this face. Borrowed entries have no heap to free, and their owner stays
// alive until all sets are replaced together on a face change. A staged new
// face cannot borrow from the current one.
static bool build_font_set(font_set_t *set, int setting, bool reuse_current) {
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		const ui_font_t *ui = &ui_fonts[i];
		int size = drawn_size(ui->size, setting);
		bool borrowed = false;
		if (reuse_current) {
			for (int other = 0; other < FONTS_TEXT_COUNT; other++) {
				if (current_sets[other].built && drawn_size(ui->size, other) == size) {
					set->fonts[i] = current_sets[other].fonts[i];
					borrowed = true;
					break;
				}
			}
		}
		if (!borrowed && !open_chain(ui, size, set, i)) {
			return false;
		}
	}
	return true;
}

// A size of the face in force, built on first use.
static bool build_current_size(int setting) {
	if (current_sets[setting].built) {
		return true;
	}
	font_set_t *set = &current_sets[setting];
	if (!build_font_set(set, setting, true)) {
		free_font_set(set);
		return false;
	}
	set->built = true;
	return true;
}

// Fills the objects the interface points at, from the set for the size in
// force.
static void copy_into_objects(const font_set_t *set) {
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		*ui_fonts[i].font = set->fonts[i];
	}
}

// The line height every named font is drawing at, before a change: a label
// whose height was worked out from a font (one line, or two, cut with
// LV_LABEL_LONG_DOT) is found by that value afterwards, and gets the same
// number of lines at the new one.
static void measure_line_heights(int32_t *out) {
	for (size_t i = 0; i < UI_FONT_COUNT; i++) {
		out[i] = lv_font_get_line_height(ui_fonts[i].font);
	}
}

// ---------------------------------------------------------------------------
// Changing the face or the size on a running interface
//
// Every label draws through the objects above, so filling them from another
// set is most of the change: after that each object is told its style has
// changed, and LVGL measures the text again and lays the pages out around it.
//
// What LVGL cannot know about is a height a page worked out from a font when
// it was built -- a label pinned to one line, or capped at two, so that
// LV_LABEL_LONG_DOT cuts it there. Those are found by their value: a label
// whose height, or maximum height, is a whole number of its own font's lines
// (with the line spacing between them) is a label sized that way, and gets the
// same number of lines at the new size. Anything else the pages worked out
// from a font is theirs to redo, from a callback registered below.
// ---------------------------------------------------------------------------

#define FONTS_CHANGE_CALLBACKS 8
static void (*change_callbacks[FONTS_CHANGE_CALLBACKS])(void);
static int change_callback_count;

void fonts_register_change(void (*cb)(void)) {
	if (cb && change_callback_count < FONTS_CHANGE_CALLBACKS) {
		change_callbacks[change_callback_count++] = cb;
	}
}

// `value` again for the new line height, when it is one to three lines of the
// old one. Unchanged otherwise.
static int32_t rescale_lines(int32_t value, int32_t old_line, int32_t new_line, int32_t gap) {
	if (!LV_COORD_IS_PX(value) || old_line <= 0) {
		return value;
	}
	for (int32_t lines = 1; lines <= 3; lines++) {
		if (value == lines * old_line + (lines - 1) * gap) {
			return lines * new_line + (lines - 1) * gap;
		}
	}
	return value;
}

static void rescale_labels(lv_obj_t *obj, const int32_t *old_line) {
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
		for (size_t i = 0; i < UI_FONT_COUNT; i++) {
			if (ui_fonts[i].font != font) {
				continue;
			}
			int32_t new_line = lv_font_get_line_height(font);
			if (new_line == old_line[i]) {
				break;
			}
			int32_t gap = lv_obj_get_style_text_line_space(obj, LV_PART_MAIN);
			int32_t height = lv_obj_get_style_height(obj, LV_PART_MAIN);
			int32_t scaled = rescale_lines(height, old_line[i], new_line, gap);
			if (scaled != height) {
				lv_obj_set_height(obj, scaled);
			}
			int32_t max_height = lv_obj_get_style_max_height(obj, LV_PART_MAIN);
			scaled = rescale_lines(max_height, old_line[i], new_line, gap);
			if (scaled != max_height) {
				lv_obj_set_style_max_height(obj, scaled, 0);
			}
			break;
		}
	}
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		rescale_labels(lv_obj_get_child(obj, (int32_t)i), old_line);
	}
}

static void refresh_interface(const int32_t *old_line) {
	// Every screen the display holds, the top and system layers included:
	// LVGL keeps those in the same list.
	for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
		for (uint32_t i = 0; i < disp->screen_cnt; i++) {
			rescale_labels(disp->screens[i], old_line);
			lv_obj_refresh_style(disp->screens[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
		}
	}
	for (int i = 0; i < change_callback_count; i++) {
		change_callbacks[i]();
	}
}

bool fonts_set_text_size(int setting) {
	if (setting < 0 || setting >= FONTS_TEXT_COUNT) {
		return false;
	}
	if (setting == text_size) {
		return true;
	}
	if (!build_current_size(setting)) {
		return false;
	}

	int32_t old_line[UI_FONT_COUNT];
	measure_line_heights(old_line);
	copy_into_objects(&current_sets[setting]);
	text_size = setting;

	refresh_interface(old_line);
	fprintf(stderr, "fonts: %s text\n", setting == FONTS_TEXT_LARGE ? "large" :
			setting == FONTS_TEXT_SMALL ? "small" : "normal");
	return true;
}

const char *fonts_choice(void) { return active.id; }

int fonts_choices(const char **ids, const char **labels, int max) {
	int count = 0;
	for (size_t i = 0; i < FACE_COUNT && count < max; i++) {
		if (!first_present(faces[i].regular_files)) {
			continue;
		}
		ids[count] = faces[i].id;
		labels[count] = faces[i].label;
		count++;
	}

	card_scan();
	for (int i = 0; i < card_count && count < max; i++) {
		ids[count] = card_ids[i];
		labels[count] = card_labels[i];
		count++;
	}
	return count;
}

bool fonts_set_face(const char *id) {
	// Asked without opening anything first: a face that is already on screen
	// is left alone, file and all.
	face_t wanted;
	if (!face_resolve(id, &wanted, false)) {
		fprintf(stderr, "fonts: %s is not a face this build can draw with\n", id ? id : "(nothing)");
		return false;
	}
	if (strcmp(wanted.id, active.id) == 0) {
		return true;
	}

	if (!face_resolve(id, &wanted, true)) {
		fprintf(stderr, "fonts: %s cannot be read\n", id ? id : "(nothing)");
		return false;
	}

	pending = wanted;
	building = &pending;
	memset(&staged_set, 0, sizeof(staged_set));
	bool built = build_font_set(&staged_set, text_size, false);
	building = &active;

	if (!built) {
		free_font_set(&staged_set);
		face_discard_copy(&wanted); // the copy it made is nobody's now
		fprintf(stderr, "fonts: cannot draw with %s; leaving %s on screen\n", wanted.id, active.id);
		return false;
	}
	staged_set.built = true;

	int32_t old_line[UI_FONT_COUNT];
	measure_line_heights(old_line);

	// The old face's fonts are given back only once nothing is drawing
	// through them any more, and the objects the interface points at are
	// filled from the new set in the same breath: there is no moment where a
	// label can ask for a glyph from a font that has been handed back.
	const face_t previous = active;
	active = wanted;
	for (int i = 0; i < FONTS_TEXT_COUNT; i++) {
		free_font_set(&current_sets[i]);
	}
	current_sets[text_size] = staged_set;
	memset(&staged_set, 0, sizeof(staged_set));
	copy_into_objects(&current_sets[text_size]);

	refresh_interface(old_line);

	// The copy the face that was just left came from, if it was a card font
	// and the new face is not drawing from it.
	if (previous.card_copy[0] && strcmp(previous.card_copy, active.card_copy) != 0) {
		remove(previous.card_copy);
	}

	build_summary();
	fprintf(stderr, "fonts: face %s (%s)%s\n", active.id, basename_of(active.regular_file),
			active.card_copy[0] ? " from the card" : "");
	return true;
}

void fonts_set_card_root(const char *sd_root) {
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");

	// A card font chosen before the card could be looked at: the player has
	// mounted it by now (main.c starts the storage before the interface), so
	// the face can be built here -- normally before the first page exists,
	// which is why this is called at the top of gui_init().
	const char *stored = config_get("ui", "font", FACE_DEFAULT_ID);
	if (strncmp(stored, CARD_ID_PREFIX, sizeof(CARD_ID_PREFIX) - 1) == 0) {
		fonts_set_face(stored);
	}
}

// The face name for the log and the developer options page: the files in use,
// and the scripts chained behind them.
static void build_summary(void) {
	int len;
	if (active.fallback_regular) {
		len = snprintf(summary, sizeof(summary), "%s", face_last_file(active.regular_file));
		if (len > 0 && (size_t)len < sizeof(summary)) {
			len += snprintf(summary + len, sizeof(summary) - (size_t)len, ", fallback %s",
							face_last_file(active.fallback_regular));
		}
		if (active.fallback_bold && len > 0 && (size_t)len < sizeof(summary)) {
			len += snprintf(summary + len, sizeof(summary) - (size_t)len, ", bold fallback %s",
							face_last_file(active.fallback_bold));
		}
	} else {
		len = snprintf(summary, sizeof(summary), "%s%s%s", face_last_file(active.regular_file),
					   active.bold_file ? ", " : "", active.bold_file ? face_last_file(active.bold_file) : "");
	}
	const char *sep = ", ";
	if (japanese.used && len > 0 && (size_t)len < sizeof(summary)) {
		len += snprintf(summary + len, sizeof(summary) - (size_t)len, "%s%s", sep, japanese.name);
		sep = " + ";
	}
	for (size_t i = 0; i < SCRIPT_COUNT && len > 0 && (size_t)len < sizeof(summary); i++) {
		if (scripts[i].used) {
			len += snprintf(summary + len, sizeof(summary) - (size_t)len, "%s%s", sep, scripts[i].name);
			sep = " + ";
		}
	}
}

bool fonts_init(void) {
	const char *default_file = first_present(FONT_DEFAULT_FILES);
	if (!default_file) {
		fprintf(stderr, "fonts: no readable default.ttf/default.otf in %s or %s\n", FONT_DIR, FONT_DIR_FALLBACK);
		return false;
	}

	// The card root is not known until the storage has looked for the card,
	// which happens after this; the simulator is told where to look instead.
	// fonts_set_card_root() picks up a card face chosen before the player
	// started, as soon as the root is there.
	const char *root = getenv("MORPTHIS_SD_ROOT");
	if (root) {
		snprintf(card_root, sizeof(card_root), "%s", root);
	}

	for (size_t i = 0; i < SCRIPT_COUNT; i++) {
		scripts[i].regular = first_present(scripts[i].regular_files);
		scripts[i].bold = first_present(scripts[i].bold_files);
	}
	japanese.regular = first_present(japanese.regular_files);
	japanese.bold = first_present(japanese.bold_files);
	japanese_ui = strcmp(lang_current(), FONTS_JAPANESE_LANGUAGE) == 0;

	face_t default_face;
	face_resolve(FACE_DEFAULT_ID, &default_face, true);

	const char *stored_font = config_get("ui", "font", FACE_DEFAULT_ID);
	// Keep devices that had Roboto Mono selected on the equivalent new face.
	const char *chosen = strcmp(stored_font, "roboto_mono") == 0 ? "roboto" : stored_font;
	if (!face_resolve(chosen, &active, true)) {
		if (strcmp(chosen, FACE_DEFAULT_ID) != 0) {
			fprintf(stderr, "fonts: [ui] font = %s cannot be used; drawing with %s\n", chosen, FACE_DEFAULT_ID);
		}
		active = default_face;
	}
	building = &active;

	text_size = config_get_int("ui", "text_size", FONTS_TEXT_NORMAL);
	if (text_size < 0 || text_size >= FONTS_TEXT_COUNT) {
		text_size = FONTS_TEXT_NORMAL;
	}
	if (!build_current_size(text_size)) {
		return false;
	}
	// Fill the static objects the interface points at.
	copy_into_objects(&current_sets[text_size]);

	build_summary();

	char from[256];
	if (active.card_copy[0]) {
		snprintf(from, sizeof(from), "%s", "the card");
	} else {
		snprintf(from, sizeof(from), "%.*s", (int)(basename_of(active.regular_file) - active.regular_file - 1),
				 active.regular_file);
	}
	fprintf(stderr, "fonts: %d sizes from %s (%s)%s\n", (int)UI_FONT_COUNT, from, summary,
			text_size == FONTS_TEXT_LARGE ? ", large text" :
			text_size == FONTS_TEXT_SMALL ? ", small text" : "");
	return true;
}

void fonts_set_language(const char *name) {
	bool japanese_now = name && strcmp(name, FONTS_JAPANESE_LANGUAGE) == 0;
	if (japanese_now == japanese_ui) {
		return;
	}
	japanese_ui = japanese_now;

	// The kanji and kana already on screen change face: every label is
	// measured again.
	for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
		for (uint32_t i = 0; i < disp->screen_cnt; i++) {
			lv_obj_refresh_style(disp->screens[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
		}
	}
}

// Empty until the faces have been opened, which is before any page is built.
// The stand-in is resolved here rather than stored, so it follows the language
// the same way every other line on the page does.
const char *fonts_summary(void) { return summary[0] ? summary : tr("none"); }
