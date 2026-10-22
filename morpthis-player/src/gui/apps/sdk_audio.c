#include "sdk_internal.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/topbar.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/eq.h"
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

bool morpthis_audio_eq_enabled(void) { return eq_get_enabled(); }

void morpthis_audio_set_eq_enabled(bool enabled) { eq_set_enabled(enabled); }

int morpthis_audio_eq_band(int band) {
	return band >= 0 && band < MORPTHIS_EQ_BANDS ? eq_get_band(band) : 0;
}

bool morpthis_audio_set_eq_band(int band, int gain_db) {
	if (band < 0 || band >= MORPTHIS_EQ_BANDS) {
		return false;
	}
	eq_set_band(band, gain_db);
	return true;
}

void morpthis_audio_eq_reset(void) { eq_reset(); }

// ---------------------------------------------------------------------------
// Raw output
// ---------------------------------------------------------------------------

// Open, write and close all come from the app's mixer thread, which is why
// there is no lock here. audio_external_write() blocks at the rate the DAC
// plays, and a lock held across it would only make close wait behind it.
//
// The flags below are read by the GUI thread (the status-bar indicator and the
// app host), hence volatile. The top bar itself must only be touched on the GUI
// thread: the app can call set_playing() from its mixer, so every repaint goes
// through a posted callback rather than straight into LVGL.
static volatile bool external_open;
static int external_bits = 16;
static int external_rate;
static int external_channels;
static int external_buffer_ms;
static volatile bool app_output_active;
static volatile bool app_output_playing;

// The track the app publishes for the player's control centre. Kept under its
// own lock because an app may publish it from its mixer thread while the GUI
// reads it for the card.
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;
static morpthis_track_t app_track;
static bool app_track_valid;

static void refresh_playback_cb(void *user) {
	(void)user;
	topbar_refresh_playback();
}

static void request_playback_refresh(void) { morpthis_app_post(refresh_playback_cb, NULL); }

bool sdk_audio_app_active(void) { return app_output_active; }
bool sdk_audio_app_playing(void) { return app_output_playing; }

bool sdk_audio_app_track(morpthis_track_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&track_lock);
	bool valid = app_track_valid;
	if (valid) {
		*out = app_track;
	} else {
		memset(out, 0, sizeof(*out));
	}
	pthread_mutex_unlock(&track_lock);
	return valid;
}

// Called by the host when the app is gone. A clean shutdown has already closed
// the output; a force-closed app may still think it owns it, so release the DAC
// and clear the flags the status bar reads. Runs on the GUI thread.
void sdk_audio_app_reset(void) {
	if (app_output_active) {
		audio_external_end();
	}
	external_open = false;
	external_bits = 16;
	external_rate = 0;
	external_channels = 0;
	external_buffer_ms = 0;
	app_output_active = false;
	app_output_playing = false;
	audio_external_set_playing(false);
	pthread_mutex_lock(&track_lock);
	memset(&app_track, 0, sizeof(app_track));
	app_track_valid = false;
	pthread_mutex_unlock(&track_lock);
	topbar_refresh_playback();
}

bool morpthis_audio_open_ex(int rate, int channels, int bits, int buffer_ms) {
	if (external_open) {
		if (external_rate == rate && external_channels == channels && external_bits == bits &&
			external_buffer_ms == buffer_ms) {
			return true;
		}
		// A music app changing tracks has to be able to change the terms; the
		// device cannot re-clock an open PCM, so close and reopen it.
		morpthis_audio_close();
	}
	if (rate <= 0 || channels < 1 || channels > 2 || buffer_ms < 0 ||
		(bits != 16 && bits != 24 && bits != 32)) {
		return false;
	}

	// One writer on the DAC. The queue is kept, so the player can resume the
	// track it was on once the app has closed the output.
	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}

	external_bits = bits;
	external_open = audio_external_begin_latency(rate, channels, bits, buffer_ms);
	if (external_open) {
		external_rate = rate;
		external_channels = channels;
		external_buffer_ms = buffer_ms;
		app_output_active = true;
		app_output_playing = true;
		audio_external_set_playing(true);
		request_playback_refresh();
	}
	return external_open;
}

bool morpthis_audio_open(int rate, int channels, int buffer_ms) {
	return morpthis_audio_open_ex(rate, channels, 16, buffer_ms);
}

size_t morpthis_audio_write_pcm(const void *pcm, size_t frames) {
	if (!external_open || !pcm || frames == 0) {
		return 0;
	}
	if (frames > (size_t)INT_MAX) {
		frames = (size_t)INT_MAX;
	}
	int written = audio_external_write(pcm, (int)frames);
	return written > 0 ? (size_t)written : 0;
}

size_t morpthis_audio_write(const int16_t *pcm, size_t frames) {
	if (external_bits != 16) {
		return 0; // use morpthis_audio_write_pcm for 24/32-bit output
	}
	return morpthis_audio_write_pcm(pcm, frames);
}

void morpthis_audio_set_playing(bool playing) {
	if (!external_open) {
		return;
	}
	app_output_playing = playing;
	audio_external_set_playing(playing);
	request_playback_refresh();
}

void morpthis_audio_set_track(const morpthis_track_t *track, bool playing) {
	pthread_mutex_lock(&track_lock);
	if (track) {
		app_track = *track;
		app_track_valid = true;
	} else {
		memset(&app_track, 0, sizeof(app_track));
		app_track_valid = false;
	}
	pthread_mutex_unlock(&track_lock);

	if (external_open) {
		app_output_playing = playing;
		audio_external_set_playing(playing);
		request_playback_refresh();
	}
}

void morpthis_audio_close(void) {
	if (!external_open) {
		return;
	}
	audio_external_end();
	external_open = false;
	external_bits = 16;
	external_rate = 0;
	external_channels = 0;
	external_buffer_ms = 0;
	app_output_active = false;
	app_output_playing = false;
	audio_external_set_playing(false);
	pthread_mutex_lock(&track_lock);
	memset(&app_track, 0, sizeof(app_track));
	app_track_valid = false;
	pthread_mutex_unlock(&track_lock);
	request_playback_refresh();
}
