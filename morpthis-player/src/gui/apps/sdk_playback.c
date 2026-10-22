#include "sdk_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/topbar.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

// ---------------------------------------------------------------------------
// playback.h
// ---------------------------------------------------------------------------

static playback_mode_t mode_to_internal(morpthis_playback_mode_t mode) {
	switch (mode) {
	case MORPTHIS_PLAYBACK_REPEAT_ALL:
		return PLAYBACK_MODE_REPEAT_ALL;
	case MORPTHIS_PLAYBACK_REPEAT_ONE:
		return PLAYBACK_MODE_REPEAT_ONE;
	case MORPTHIS_PLAYBACK_SHUFFLE:
		return PLAYBACK_MODE_SHUFFLE;
	case MORPTHIS_PLAYBACK_SHUFFLE_REPEAT:
		return PLAYBACK_MODE_SHUFFLE_REPEAT;
	case MORPTHIS_PLAYBACK_NORMAL:
	default:
		return PLAYBACK_MODE_NORMAL;
	}
}

static morpthis_playback_mode_t mode_from_internal(playback_mode_t mode) {
	switch (mode) {
	case PLAYBACK_MODE_REPEAT_ALL:
		return MORPTHIS_PLAYBACK_REPEAT_ALL;
	case PLAYBACK_MODE_REPEAT_ONE:
		return MORPTHIS_PLAYBACK_REPEAT_ONE;
	case PLAYBACK_MODE_SHUFFLE:
		return MORPTHIS_PLAYBACK_SHUFFLE;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		return MORPTHIS_PLAYBACK_SHUFFLE_REPEAT;
	case PLAYBACK_MODE_NORMAL:
	default:
		return MORPTHIS_PLAYBACK_NORMAL;
	}
}

static int seconds_to_ms(double seconds) {
	if (seconds <= 0.0) {
		return 0;
	}
	double ms = seconds * 1000.0;
	return ms > (double)INT_MAX ? INT_MAX : (int)(ms + 0.5);
}

static void fill_track_text(morpthis_playback_info_t *out, const device_state_t *state) {
	snprintf(out->title, sizeof(out->title), "%s", state->metadata.title);
	snprintf(out->artist, sizeof(out->artist), "%s", state->metadata.artist);
	snprintf(out->album, sizeof(out->album), "%s", state->metadata.album);
	if (!out->title[0]) {
		const char *slash = strrchr(state->current_file, '/');
		snprintf(out->title, sizeof(out->title), "%s", slash ? slash + 1 : state->current_file);
	}
}

bool morpthis_playback_play_file(const char *absolute_path, int start_ms) {
	if (!absolute_path || !absolute_path[0] || absolute_path[0] != '/' || access(absolute_path, R_OK) != 0) {
		return false;
	}
	if (start_ms < 0) {
		start_ms = 0;
	}
	if (start_ms > 0) {
		device_state_play_file_at(absolute_path, (double)start_ms / 1000.0);
	} else {
		device_state_play_file(absolute_path);
	}
	topbar_refresh_playback();
	return true;
}

bool morpthis_playback_play_queue(const char *const *paths, int count, int start_index) {
	if (!paths || count <= 0 || start_index < 0 || start_index >= count) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		if (!paths[i] || paths[i][0] != '/') {
			return false;
		}
	}
	device_state_play_list(paths, count, start_index);
	topbar_refresh_playback();
	return true;
}

bool morpthis_playback_play_folder(const char *absolute_folder, const char *selected_name) {
	if (!absolute_folder || absolute_folder[0] != '/' || !selected_name || !selected_name[0] ||
		strchr(selected_name, '/') || strchr(selected_name, '\\')) {
		return false;
	}
	char path[MORPTHIS_PLAYBACK_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s", absolute_folder, selected_name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return false;
	}
	return morpthis_playback_play_file(path, 0);
}

bool morpthis_playback_play_index(int index) {
	return index >= 0 && device_state_play_queue_index(index);
}

bool morpthis_playback_resume(void) {
	device_state_t state;
	device_state_get(&state);
	if (!state.live && !state.current_file[0]) {
		return false;
	}
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		device_state_toggle_play_pause();
	}
	return true;
}

bool morpthis_playback_pause(void) {
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audio_pause();
	}
	return true;
}

bool morpthis_playback_toggle(void) {
	device_state_toggle_play_pause();
	return true;
}

bool morpthis_playback_stop(void) {
	device_state_stop();
	return true;
}

bool morpthis_playback_next(void) {
	return device_state_next(NULL, 0);
}

bool morpthis_playback_prev(void) {
	return device_state_prev(NULL, 0);
}

bool morpthis_playback_seek(int position_ms) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return false;
	}
	if (position_ms < 0) {
		position_ms = 0;
	}
	device_state_seek((double)position_ms / 1000.0);
	return true;
}

bool morpthis_playback_get_info(morpthis_playback_info_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	device_state_t state;
	device_state_get(&state);

	snprintf(out->path, sizeof(out->path), "%s", state.current_file);
	fill_track_text(out, &state);

	char codec[MORPTHIS_PLAYBACK_CODEC_MAX];
	codec[0] = '\0';
	audio_get_stream_codec(codec, sizeof(codec));
	snprintf(out->codec, sizeof(out->codec), "%s", codec);

	audio_get_stream_info(&out->sample_rate, &out->channels);
	out->bit_depth = audio_get_stream_bits();
	out->bitrate_kbps = audio_get_stream_bitrate_kbps();
	out->dsd_multiple = audio_get_dsd_multiple();
	out->lossy = audio_stream_is_lossy();
	out->live = state.live;

	out->position_ms = seconds_to_ms(state.progress_current_secs);
	out->duration_ms = seconds_to_ms(state.progress_total_secs);

	out->queue_index = playlist_current_index();
	out->queue_count = playlist_count();
	out->mode = mode_from_internal(playlist_get_mode());
	out->state = morpthis_audio_state();

	return state.live || state.current_file[0] != '\0';
}

void morpthis_playback_set_mode(morpthis_playback_mode_t mode) {
	if (mode < MORPTHIS_PLAYBACK_NORMAL || mode > MORPTHIS_PLAYBACK_SHUFFLE_REPEAT) {
		return;
	}
	playlist_set_mode(mode_to_internal(mode));
}

morpthis_playback_mode_t morpthis_playback_get_mode(void) {
	return mode_from_internal(playlist_get_mode());
}

bool morpthis_playback_set_speed(double factor) {
	if (factor < 0.25 || factor > 4.0) {
		return false;
	}
	audio_set_speed(factor);
	return true;
}

double morpthis_playback_get_speed(void) {
	return audio_get_speed();
}

int morpthis_playback_queue_count(void) {
	return playlist_count();
}

int morpthis_playback_queue_index(void) {
	return playlist_current_index();
}

bool morpthis_playback_queue_path(int index, char *out, size_t size) {
	if (!out || size == 0 || index < 0) {
		return false;
	}
	return playlist_path_at(index, out, size);
}

bool morpthis_playback_queue_insert(const char *absolute_path, int slot) {
	if (!absolute_path || !absolute_path[0] || absolute_path[0] != '/') {
		return false;
	}
	bool ok = slot < 0 ? playlist_insert_next(absolute_path) : playlist_insert_at(absolute_path, slot);
	if (ok) {
		device_state_queue_changed();
	}
	return ok;
}

bool morpthis_playback_queue_clear(void) {
	playlist_clear();
	device_state_queue_changed();
	return true;
}

bool morpthis_playback_cover_path(char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	const char *path = player_cover_path();
	if (!path || !path[0]) {
		return false;
	}
	int n = snprintf(out, size, "%s", path);
	return n >= 0 && (size_t)n < size;
}

// ---------------------------------------------------------------------------
// Change notification
// ---------------------------------------------------------------------------

typedef struct {
	char path[MORPTHIS_PLAYBACK_PATH_MAX];
	morpthis_audio_state_t state;
	int index;
	int count;
	morpthis_playback_mode_t mode;
	bool valid;
} playback_marker_t;

static lv_timer_t *changed_timer;
static morpthis_playback_changed_cb changed_cb;
static void *changed_user;
static playback_marker_t changed_marker;

static void marker_take(playback_marker_t *marker) {
	morpthis_playback_info_t info;
	bool loaded = morpthis_playback_get_info(&info);
	marker->valid = loaded;
	snprintf(marker->path, sizeof(marker->path), "%s", info.path);
	marker->state = info.state;
	marker->index = info.queue_index;
	marker->count = info.queue_count;
	marker->mode = info.mode;
}

static bool marker_equal(const playback_marker_t *a, const playback_marker_t *b) {
	return a->valid == b->valid && a->state == b->state && a->index == b->index &&
		   a->count == b->count && a->mode == b->mode && strcmp(a->path, b->path) == 0;
}

static void changed_tick(lv_timer_t *timer) {
	(void)timer;
	if (!sdk_app || !sdk_app->alive) {
		return;
	}
	playback_marker_t now;
	marker_take(&now);
	if (marker_equal(&now, &changed_marker)) {
		return;
	}
	changed_marker = now;
	if (changed_cb) {
		changed_cb(changed_user);
	}
}

void morpthis_playback_set_changed_cb(morpthis_playback_changed_cb cb, void *user) {
	changed_cb = cb;
	changed_user = user;
	if (!cb) {
		if (changed_timer) {
			lv_timer_delete(changed_timer);
			changed_timer = NULL;
		}
		return;
	}

	marker_take(&changed_marker);
	if (!changed_timer) {
		changed_timer = lv_timer_create(changed_tick, 500, NULL);
	}
}

// Called from the host's teardown, on the GUI thread, before the shared object
// is unmapped. A timer left behind would call into code that is gone.
void sdk_playback_app_reset(void) {
	changed_cb = NULL;
	changed_user = NULL;
	memset(&changed_marker, 0, sizeof(changed_marker));
	if (changed_timer) {
		lv_timer_delete(changed_timer);
		changed_timer = NULL;
	}
}
