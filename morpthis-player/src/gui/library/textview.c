#include "textview.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/text/textfile.h"

lv_obj_t *textview_screen;

// The text goes into many labels of about this size rather than one: LVGL
// walks a label's text from the start on every redraw, and a page of a hundred
// kilobytes in one label would be slow to scroll.
#define CHUNK_BYTES 1500

static lv_obj_t *title_label;
static lv_obj_t *page;

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------
//
// The decoding lives in src/system/text/textfile.c: the note editor reads and
// writes the same files, and neither page owns the rules.

static lv_obj_t *add_label(const char *text, bool dim) {
	lv_obj_t *label = lv_label_create(page);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_label_set_text(label, text);
	return label;
}

// Cuts `text` into labels of about CHUNK_BYTES, at a line break when there is
// one, otherwise at a space, otherwise at the start of a character.
static void fill(char *text) {
	char *p = text;
	while (*p) {
		size_t left = strlen(p);
		size_t cut = left;
		if (left > CHUNK_BYTES) {
			cut = CHUNK_BYTES;
			size_t i = cut;
			while (i > CHUNK_BYTES / 2 && p[i - 1] != '\n') {
				i--;
			}
			if (p[i - 1] != '\n') {
				i = cut;
				while (i > CHUNK_BYTES / 2 && p[i - 1] != ' ') {
					i--;
				}
				if (p[i - 1] != ' ') {
					i = cut;
					while (i > 0 && ((unsigned char)p[i] & 0xC0) == 0x80) {
						i--;
					}
				}
			}
			cut = i;
		}

		// The label gets the piece without the line break it ends on: the next
		// label starts on a line of its own anyway.
		char saved = p[cut];
		p[cut] = '\0';
		size_t shown = cut;
		if (shown > 0 && p[shown - 1] == '\n') {
			p[shown - 1] = '\0';
		}
		add_label(p, false);
		p[cut] = saved;
		p += cut;
	}
}

bool textview_open_text(const char *title, const char *text, const char *footer, bool truncated) {
	if (!title || !text) {
		return false;
	}

	lv_label_set_text(title_label, title);

	lv_obj_clean(page);
	if (text[0]) {
		// One label per page of text: fill() cuts the text up, so the copy
		// here is the page's to keep and it is handed a writable buffer.
		char *copy = malloc(strlen(text) + 1);
		if (!copy) {
			return false;
		}
		strcpy(copy, text);
		fill(copy);
		free(copy);
	} else {
		add_label(tr("files_text_empty"), true);
	}
	if (truncated) {
		add_label(tr("files_text_truncated"), true);
	} else if (footer && footer[0]) {
		add_label(footer, true);
	}

	lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
	switch_screen(textview_screen);
	return true;
}

bool textview_open(const char *path) {
	unsigned char *raw = NULL;
	size_t len = 0;
	bool more = false;
	if (!textfile_read(path, TEXTVIEW_MAX_BYTES, &raw, &len, &more)) {
		return false;
	}

	char *text = textfile_decode(raw, len);
	free(raw);
	if (!text) {
		return false;
	}

	const char *slash = strrchr(path, '/');
	bool ok = textview_open_text(slash ? slash + 1 : path, text, NULL, more);
	free(text);
	return ok;
}

void textview_init(gui_config_t *cfg) {
	lv_obj_add_style(textview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(textview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);

	page = lv_obj_create(textview_screen);
	lv_obj_set_size(page, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_radius(page, 0, 0);
	lv_obj_set_style_pad_hor(page, cfg->padding, 0);
	lv_obj_set_style_pad_top(page, 0, 0);
	lv_obj_set_style_pad_bottom(page, cfg->padding * 2, 0);
	lv_obj_set_style_pad_row(page, 0, 0);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(page, true);

	switcher_attach_back_gesture(textview_screen);
}
