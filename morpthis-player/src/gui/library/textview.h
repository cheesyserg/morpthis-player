#ifndef TEXTVIEW_H
#define TEXTVIEW_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A plain-text file on a page of its own, read-only: the file manager opens
// .txt, .lrc, .nfo and .ini files here.
//
// The text is shown as it is, wrapped to the screen. UTF-8 (with or without a
// byte order mark) and UTF-16 with a byte order mark are read as such; anything
// else that is not valid UTF-8 is taken for Windows-1252, which is what a file
// saved on Windows as "ANSI" is. Only the first TEXTVIEW_MAX_BYTES of a longer
// file are shown, with a line at the end saying so.

#define TEXTVIEW_MAX_BYTES (128 * 1024)

extern lv_obj_t *textview_screen;

void textview_init(gui_config_t *cfg);

// Reads `path` and switches to the page. False, staying where it is, when the
// file cannot be read.
bool textview_open(const char *path);

// Shows text the caller already holds -- a note opened from the notes shelf,
// read out of its folder by the notes store -- under `title` rather than a file
// name. `footer`, when it is not NULL, is a line of its own at the end, and
// `truncated` puts the viewer's own notice there instead. False when `text` is
// NULL.
bool textview_open_text(const char *title, const char *text, const char *footer, bool truncated);

#endif /* TEXTVIEW_H */
