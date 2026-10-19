#ifndef AUDIOBOOKEXTRAS_H
#define AUDIOBOOKEXTRAS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The audiobook pages beyond the shelves: the bookmarks -- the books that have
// any, then one book's -- and a book's summary.
//
// Reached two ways. The section page's corner button opens the books that have
// bookmarks; the player's menu on a book opens that book's bookmarks or its
// summary directly, and backing out of either returns to the player.

void audiobookextras_init(gui_config_t *cfg);

// The books that have bookmarks.
void audiobookextras_open_bookmarks(void);

// For the player's menu, about the book playing now.
void audiobookextras_add_bookmark(void);
void audiobookextras_open_current_bookmarks(void);
void audiobookextras_open_current_summary(void);

#endif /* AUDIOBOOKEXTRAS_H */
