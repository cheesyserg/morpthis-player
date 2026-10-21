#include "sdk_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/topbar.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/radio.h"

// ---------------------------------------------------------------------------
// audio.h
// ---------------------------------------------------------------------------

static void fill_from_path(morpthis_track_t *out, const char *path) {
	if (!path || !path[0]) {
		return;
	}
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	snprintf(out->title, sizeof(out->title), "%s", name);
}

bool morpthis_audio_current(morpthis_track_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	device_state_t state;
	device_state_get(&state);
	if (!state.live && !state.current_file[0]) {
		return false;
	}

	snprintf(out->title, sizeof(out->title), "%s", state.metadata.title);
	snprintf(out->artist, sizeof(out->artist), "%s", state.metadata.artist);
	snprintf(out->album, sizeof(out->album), "%s", state.metadata.album);
	if (!out->title[0]) {
		fill_from_path(out, state.current_file);
	}
	if (!state.live && state.progress_total_secs > 0.0) {
		double ms = state.progress_total_secs * 1000.0;
		out->duration_ms = ms > (double)INT_MAX ? INT_MAX : (int)(ms + 0.5);
	}
	return true;
}

morpthis_audio_state_t morpthis_audio_state(void) {
	switch (audio_get_status()) {
	case AUDIO_STATUS_PLAYING:
		return MORPTHIS_AUDIO_PLAYING;
	case AUDIO_STATUS_PAUSED:
		return MORPTHIS_AUDIO_PAUSED;
	case AUDIO_STATUS_STOPPED:
	default:
		return MORPTHIS_AUDIO_STOPPED;
	}
}

bool morpthis_audio_play(void) {
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		return true;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.live && !state.current_file[0]) {
		return false;
	}
	// From paused this resumes; from stopped it starts the loaded track again.
	device_state_toggle_play_pause();
	return true;
}

bool morpthis_audio_pause(void) {
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audio_pause();
	}
	return true;
}

bool morpthis_audio_next(void) {
	return device_state_next(NULL, 0);
}

bool morpthis_audio_prev(void) {
	return device_state_prev(NULL, 0);
}

int morpthis_audio_volume(void) {
	return get_volume_percent();
}

bool morpthis_audio_set_volume(int volume) {
	if (volume < 0) {
		volume = 0;
	}
	if (volume > 100) {
		volume = 100;
	}
	set_volume_percent(volume);
	topbar_refresh_volume(volume);
	return true;
}

// ---------------------------------------------------------------------------
// Raw output
// ---------------------------------------------------------------------------

// Open, write and close all come from the app's mixer thread, which is why
// there is no lock here. audio_external_write() blocks at the rate the DAC
// plays, and a lock held across it would only make close wait behind it.
static bool external_open;

bool morpthis_audio_open(int rate, int channels, int buffer_ms) {
	if (external_open) {
		return true;
	}
	if (rate <= 0 || channels < 1 || channels > 2 || buffer_ms < 0) {
		return false;
	}

	// One writer on the DAC. The queue is kept, so the player can resume the
	// track it was on once the app has closed the output.
	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}

	external_open = audio_external_begin_latency(rate, channels, 16, buffer_ms);
	return external_open;
}

size_t morpthis_audio_write(const int16_t *pcm, size_t frames) {
	if (!external_open || !pcm || frames == 0) {
		return 0;
	}
	if (frames > (size_t)INT_MAX) {
		frames = (size_t)INT_MAX;
	}
	int written = audio_external_write(pcm, (int)frames);
	return written > 0 ? (size_t)written : 0;
}

void morpthis_audio_close(void) {
	if (!external_open) {
		return;
	}
	audio_external_end();
	external_open = false;
}
