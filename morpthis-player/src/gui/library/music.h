#ifndef MUSIC_H
#define MUSIC_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

typedef enum {
	MUSIC_TILE_ALL_TRACKS = 0,
	MUSIC_TILE_ALBUMS,
	MUSIC_TILE_ARTISTS,
	MUSIC_TILE_ALBUM_ARTISTS,
	MUSIC_TILE_GENRES,
	MUSIC_TILE_YEARS,
	MUSIC_TILE_PLAYLISTS,
	MUSIC_TILE_MOST_PLAYED,
	MUSIC_TILE_BROWSE,
	MUSIC_TILE_COUNT,
} music_tile_id_t;

// The Music page: the library's index tiles (all tracks, albums, artists,
// album artists, genres, years, playlists, most played) plus the file browser,
// with library settings, playlists, favourites and search on the corner buttons.
extern lv_obj_t *music_screen;

void music_init(gui_config_t *cfg);

// Rebuilds the Music page tile grid from the current tile arrangement and
// updates the corner button icon. Called at startup and whenever the user
// rearranges tiles or toggles "playlists first".
void music_refresh_layout(void);

#endif /* MUSIC_H */
