#include "sdk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/library/library.h"

// ---------------------------------------------------------------------------
// library.h
// ---------------------------------------------------------------------------

static library_list_t list_to_internal(morpthis_library_list_t list) {
	switch (list) {
	case MORPTHIS_LIBRARY_ALBUMS:
		return LIBRARY_LIST_ALBUMS;
	case MORPTHIS_LIBRARY_ARTISTS:
		return LIBRARY_LIST_ARTISTS;
	case MORPTHIS_LIBRARY_ALBUM_ARTISTS:
		return LIBRARY_LIST_ALBUM_ARTISTS;
	case MORPTHIS_LIBRARY_GENRES:
		return LIBRARY_LIST_GENRES;
	case MORPTHIS_LIBRARY_FAVOURITES:
		return LIBRARY_LIST_FAVOURITES;
	case MORPTHIS_LIBRARY_PLAYLIST:
		return LIBRARY_LIST_PLAYLIST;
	case MORPTHIS_LIBRARY_YEARS:
		return LIBRARY_LIST_YEARS;
	case MORPTHIS_LIBRARY_MOST_PLAYED:
		return LIBRARY_LIST_MOST_PLAYED;
	case MORPTHIS_LIBRARY_TRACKS:
	default:
		return LIBRARY_LIST_TRACKS;
	}
}

static library_filter_t filter_to_internal(morpthis_library_filter_t filter) {
	switch (filter) {
	case MORPTHIS_LIBRARY_FILTER_ALBUM:
		return LIBRARY_FILTER_ALBUM;
	case MORPTHIS_LIBRARY_FILTER_ARTIST:
		return LIBRARY_FILTER_ARTIST;
	case MORPTHIS_LIBRARY_FILTER_ALBUM_ARTIST:
		return LIBRARY_FILTER_ALBUM_ARTIST;
	case MORPTHIS_LIBRARY_FILTER_GENRE:
		return LIBRARY_FILTER_GENRE;
	case MORPTHIS_LIBRARY_FILTER_SEARCH:
		return LIBRARY_FILTER_SEARCH;
	case MORPTHIS_LIBRARY_FILTER_YEAR:
		return LIBRARY_FILTER_YEAR;
	case MORPTHIS_LIBRARY_FILTER_NONE:
	default:
		return LIBRARY_FILTER_NONE;
	}
}

static library_order_t order_to_internal(morpthis_library_order_t order) {
	switch (order) {
	case MORPTHIS_LIBRARY_ORDER_ALBUM:
		return LIBRARY_ORDER_ALBUM;
	case MORPTHIS_LIBRARY_ORDER_ADDED:
		return LIBRARY_ORDER_ADDED;
	case MORPTHIS_LIBRARY_ORDER_YEAR:
		return LIBRARY_ORDER_YEAR;
	case MORPTHIS_LIBRARY_ORDER_YEAR_DESC:
		return LIBRARY_ORDER_YEAR_DESC;
	case MORPTHIS_LIBRARY_ORDER_DEFAULT:
	default:
		return LIBRARY_ORDER_DEFAULT;
	}
}

static morpthis_library_quality_t quality_from_internal(library_quality_t quality) {
	switch (quality) {
	case LIBRARY_QUALITY_LOSSY:
		return MORPTHIS_LIBRARY_QUALITY_LOSSY;
	case LIBRARY_QUALITY_CD:
		return MORPTHIS_LIBRARY_QUALITY_CD;
	case LIBRARY_QUALITY_HIFI:
		return MORPTHIS_LIBRARY_QUALITY_HIFI;
	case LIBRARY_QUALITY_DSD:
		return MORPTHIS_LIBRARY_QUALITY_DSD;
	case LIBRARY_QUALITY_NONE:
	default:
		return MORPTHIS_LIBRARY_QUALITY_NONE;
	}
}

typedef struct {
	morpthis_library_row_cb cb;
	void *user;
} row_context_t;

static bool row_trampoline(const char *name, const char *path, const char *artist, void *user) {
	row_context_t *ctx = user;
	return ctx->cb ? ctx->cb(name ? name : "", path, artist, ctx->user) : false;
}

typedef struct {
	morpthis_library_search_cb cb;
	void *user;
} search_context_t;

static morpthis_library_search_type_t search_type_from_internal(library_search_type_t type) {
	switch (type) {
	case LIBRARY_SEARCH_ALBUM:
		return MORPTHIS_LIBRARY_SEARCH_ALBUM;
	case LIBRARY_SEARCH_ARTIST:
		return MORPTHIS_LIBRARY_SEARCH_ARTIST;
	case LIBRARY_SEARCH_TRACK:
	default:
		return MORPTHIS_LIBRARY_SEARCH_TRACK;
	}
}

static void search_trampoline(library_search_type_t type, const char *name, const char *path, void *user) {
	search_context_t *ctx = user;
	if (ctx->cb) {
		ctx->cb(search_type_from_internal(type), name ? name : "", path, ctx->user);
	}
}

bool morpthis_library_available(void) {
	return library_is_open();
}

int morpthis_library_track_count(void) {
	return library_track_count();
}

bool morpthis_library_scan_start(void) {
	const char *root = morpthis_fs_card_root();
	if (!root || !root[0]) {
		return false;
	}
	return library_scan_start(root);
}

void morpthis_library_scan_stop(void) {
	library_scan_stop();
}

bool morpthis_library_scan_running(void) {
	return library_scan_running();
}

int morpthis_library_scan_found(void) {
	return library_scan_found();
}

bool morpthis_library_scan_current(char *folder_out, size_t folder_size, char *file_out, size_t file_size) {
	char folder[512] = "";
	char file[512] = "";
	library_scan_current_folder(folder, sizeof(folder));
	library_scan_current_file(file, sizeof(file));
	if (folder_out && folder_size) {
		snprintf(folder_out, folder_size, "%s", folder);
	}
	if (file_out && file_size) {
		snprintf(file_out, file_size, "%s", file);
	}
	return folder[0] != '\0' || file[0] != '\0';
}

int morpthis_library_for_each(morpthis_library_list_t list, morpthis_library_filter_t filter,
							  const char *value, morpthis_library_row_cb cb, void *user) {
	if (!cb) {
		return 0;
	}
	row_context_t ctx = {cb, user};
	return library_for_each(list_to_internal(list), filter_to_internal(filter), value, row_trampoline, &ctx);
}

int morpthis_library_for_each_ordered(morpthis_library_list_t list, morpthis_library_filter_t filter,
									  const char *value, morpthis_library_order_t order,
									  morpthis_library_row_cb cb, void *user) {
	if (!cb) {
		return 0;
	}
	row_context_t ctx = {cb, user};
	return library_for_each_ordered(list_to_internal(list), filter_to_internal(filter), value,
									order_to_internal(order), row_trampoline, &ctx);
}

bool morpthis_library_track_title(const char *path, char *out, size_t size) {
	return path && out && size && library_track_title(path, out, size);
}

bool morpthis_library_track_names(const char *path, char *title, size_t title_size,
								  char *artist, size_t artist_size) {
	return path && title && title_size && artist && artist_size &&
		   library_track_names(path, title, title_size, artist, artist_size);
}

bool morpthis_library_track_album(const char *path, char *out, size_t size) {
	return path && out && size && library_track_album(path, out, size);
}

bool morpthis_library_track_album_value(const char *path, char *out, size_t size) {
	return path && out && size && library_track_album_value(path, out, size);
}

bool morpthis_library_album_title(const char *album_value, char *out, size_t size) {
	if (!album_value || !out || size == 0) {
		return false;
	}
	library_album_title(album_value, out, size);
	return out[0] != '\0';
}

bool morpthis_library_album_artist(const char *album_value, char *out, size_t size) {
	return album_value && out && size && library_album_artist(album_value, out, size);
}

morpthis_library_quality_t morpthis_library_track_quality(const char *path, int *sample_rate, int *bit_depth) {
	if (!path) {
		return MORPTHIS_LIBRARY_QUALITY_NONE;
	}
	return quality_from_internal(library_track_quality(path, sample_rate, bit_depth));
}

int morpthis_library_search(const char *query, int per_category, morpthis_library_search_cb cb, void *user) {
	if (!query || !query[0] || !cb) {
		return 0;
	}
	search_context_t ctx = {cb, user};
	return library_search(query, per_category, search_trampoline, &ctx);
}

bool morpthis_library_favourite_toggle(const char *path, const char *title, const char *artist) {
	return path && library_fav_toggle(path, title ? title : "", artist ? artist : "");
}

bool morpthis_library_is_favourite(const char *path) {
	return path && library_fav_contains(path);
}

void morpthis_library_record_play(const char *path, const char *title, const char *artist) {
	if (path) {
		library_record_play(path, title ? title : "", artist ? artist : "");
	}
}

int morpthis_library_play_count(const char *path) {
	return path ? library_play_count(path) : 0;
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

int morpthis_library_playlists(char out[][MORPTHIS_LIBRARY_PLAYLIST_NAME_MAX], int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char **names = NULL;
	int total = library_playlist_names(&names);
	int count = total > max ? max : total;
	for (int i = 0; i < count; i++) {
		snprintf(out[i], MORPTHIS_LIBRARY_PLAYLIST_NAME_MAX, "%s", names[i] ? names[i] : "");
	}
	library_playlist_names_free(names, total);
	return count;
}

bool morpthis_library_playlist_exists(const char *name) {
	return name && library_playlist_exists(name);
}

bool morpthis_library_playlist_create(const char *name) {
	return name && library_playlist_create(name);
}

bool morpthis_library_playlist_drop(const char *name) {
	return name && library_playlist_drop(name);
}

bool morpthis_library_playlist_rename(const char *name, const char *new_name) {
	return name && new_name && library_playlist_rename(name, new_name);
}

bool morpthis_library_playlist_append(const char *name, const char *path, const char *title,
									  const char *artist, int seconds) {
	if (!name || !path || !path[0]) {
		return false;
	}
	library_playlist_row_t row;
	memset(&row, 0, sizeof(row));
	snprintf(row.path, sizeof(row.path), "%s", path);
	snprintf(row.title, sizeof(row.title), "%s", title ? title : "");
	snprintf(row.artist, sizeof(row.artist), "%s", artist ? artist : "");
	row.seconds = seconds > 0 ? seconds : -1;
	row.present = access(path, F_OK) == 0;
	return library_playlist_append(name, &row);
}

bool morpthis_library_playlist_remove(const char *name, const char *path) {
	return name && path && library_playlist_remove_path(name, path);
}

bool morpthis_library_playlist_contains(const char *name, const char *path) {
	return name && path && library_playlist_contains(name, path);
}

bool morpthis_library_playlist_move(const char *name, int from, int to) {
	return name && library_playlist_move(name, from, to);
}

int morpthis_library_playlist_count(const char *name) {
	return name ? library_playlist_count(name) : 0;
}

int morpthis_library_playlist_rows(const char *name, int offset, int count,
								   morpthis_library_playlist_row_cb cb, void *user) {
	if (!name || !cb || offset < 0 || count <= 0) {
		return 0;
	}
	// One page at a time: a library_playlist_row_t is about two kilobytes and a
	// whole playlist of them is exactly the allocation this device cannot make.
	library_playlist_row_t rows[4];
	int delivered = 0;
	while (delivered < count) {
		int want = count - delivered;
		if (want > (int)(sizeof(rows) / sizeof(rows[0]))) {
			want = (int)(sizeof(rows) / sizeof(rows[0]));
		}
		int got = library_playlist_page(name, offset + delivered, want, rows);
		for (int i = 0; i < got; i++) {
			cb(rows[i].path, rows[i].title, rows[i].artist, (int)rows[i].seconds, rows[i].present, user);
		}
		delivered += got;
		if (got < want) {
			break;
		}
	}
	return delivered;
}
