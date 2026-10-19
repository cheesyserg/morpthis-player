#include "subsonicstream.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
	streambridge_t *bridge;
	char path[96];
	bool leased;
	bool error_reported;
} slot_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static slot_t slots[2];
static subsonic_entry_t *queue;
static subsonic_quality_t queue_quality;
static int queue_count;
static unsigned queue_generation;
static int wanted_slot = -1;
static char last_error[192];

bool subsonicstream_owns(const char *path) { return path && strncmp(path, "subsonic://", 11) == 0; }

static const char *extension(const subsonic_entry_t *track, subsonic_quality_t q) {
	if (q.format == SUBSONIC_FORMAT_MP3) {
		return "mp3";
	}
	if (q.format == SUBSONIC_FORMAT_OPUS) {
		return "opus";
	}
	// Raw means the original compressed file. Only forward-readable, light
	// codecs belong on this bridge. MP4/DSD/APE/Vorbis need another transport
	// or costly decoding and must be transcoded by the server instead.
	if (strcasecmp(track->suffix, "flac") == 0) {
		return "flac";
	}
	if (strcasecmp(track->suffix, "mp3") == 0) {
		return "mp3";
	}
	if (strcasecmp(track->suffix, "opus") == 0) {
		return "opus";
	}
	return NULL;
}

static int index_of(const char *path) {
	unsigned generation;
	int index, end = 0;
	if (!subsonicstream_owns(path) || sscanf(path, "subsonic://%u/%d.%*s%n", &generation, &index, &end) != 2 ||
		!end || path[end] || generation != queue_generation || index < 0 || index >= queue_count) {
		return -1;
	}
	const char *ext = extension(&queue[index], queue_quality);
	char expected[96];
	snprintf(expected, sizeof(expected), "subsonic://%u/%d.%s", generation, index, ext ? ext : "");
	return strcmp(path, expected) == 0 ? index : -1;
}

static bool allocate_slots(void) {
	for (int i = 0; i < 2; i++) {
		if (!slots[i].bridge) {
			slots[i].bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
			if (!slots[i].bridge) {
				return false;
			}
		}
	}
	return true;
}

bool subsonicstream_queue(const subsonic_entry_t *tracks, int count, int start, char paths[][96],
						 const char **pointers, int *queued, int *selected, char *error, size_t size) {
	if (error && size) {
		error[0] = '\0';
	}
	if (!tracks || !paths || !pointers || !queued || !selected || count <= 0 || count > SUBSONIC_PAGE_SIZE || start < 0 || start >= count) {
		return false;
	}
	subsonic_quality_t q = subsonic_quality();
	if (tracks[start].kind != SUBSONIC_TRACK || !extension(&tracks[start], q)) {
		if (error && size) {
			snprintf(error, size, "subsonic_raw_unsupported");
		}
		return false;
	}
	subsonic_entry_t *copy = calloc((size_t)count, sizeof(*copy));
	if (!copy) {
		return false;
	}
	int n = 0, chosen = 0;
	for (int i = 0; i < count; i++) {
		if (tracks[i].kind != SUBSONIC_TRACK || !extension(&tracks[i], q)) {
			continue;
		}
		if (i == start) {
			chosen = n;
		}
		copy[n++] = tracks[i];
	}
	pthread_mutex_lock(&lock);
	if (!allocate_slots()) {
		pthread_mutex_unlock(&lock);
		free(copy);
		if (error && size) {
			snprintf(error, size, "subsonic_out_of_memory");
		}
		return false;
	}
	for (int i = 0; i < 2; i++) {
		streambridge_cancel(slots[i].bridge);
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	free(queue);
	queue = copy;
	queue_count = n;
	queue_quality = q;
	queue_generation++;
	if (!queue_generation) {
		queue_generation++; // no ambiguity with a never-loaded queue
	}
	wanted_slot = -1;
	last_error[0] = '\0';
	for (int i = 0; i < n; i++) {
		snprintf(paths[i], 96, "subsonic://%u/%d.%s", queue_generation, i, extension(&copy[i], q));
		pointers[i] = paths[i];
	}
	*queued = n;
	*selected = chosen;
	pthread_mutex_unlock(&lock);
	return n > 0;
}

static int choose_slot(const char *path, bool prefetch) {
	int index = index_of(path);
	if (index < 0) {
		snprintf(last_error, sizeof(last_error), "subsonic_invalid_id");
		return -1;
	}
	// An already leased instance of the same path is NOT a replay. Repeat-one
	// needs the other slot, holding a new stream starting at byte zero.
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && strcmp(slots[i].path, path) == 0) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			if (prefetch && state.failed) {
				return -1;
			}
			if (!state.canceled && !state.failed) {
				if (!prefetch) {
					streambridge_promote(slots[i].bridge);
				}
				return i;
			}
		}
	}
	int free_slot = -1;
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && (prefetch ? i != wanted_slot : true)) {
			free_slot = i;
			if (!slots[i].path[0]) {
				break;
			}
		}
	}
	if (free_slot < 0) {
		return -1;
	}
	char url[SUBSONIC_URL_MAX], error[192] = "";
	if (!subsonic_stream_url(queue[index].id, queue_quality, url, sizeof(url), error, sizeof(error)) ||
		!streambridge_start(slots[free_slot].bridge, url, prefetch)) {
		snprintf(last_error, sizeof(last_error), "%s", error[0] ? error : "subsonic_stream_failed");
		return -1;
	}
	memset(url, 0, sizeof(url));
	snprintf(slots[free_slot].path, sizeof(slots[free_slot].path), "%s", path);
	slots[free_slot].error_reported = false;
	return free_slot;
}

bool subsonicstream_prepare(const char *path) {
	if (!subsonicstream_owns(path)) {
		subsonicstream_cancel();
		return true;
	}
	pthread_mutex_lock(&lock);
	wanted_slot = choose_slot(path, false);
	if (wanted_slot < 0 && !last_error[0]) {
		snprintf(last_error, sizeof(last_error), "subsonic_stream_failed");
	}
	// Any unused, stale pre-cache gives way. There is never a third stream.
	for (int i = 0; i < 2; i++) {
		if (i != wanted_slot && !slots[i].leased) {
			streambridge_cancel(slots[i].bridge);
			slots[i].path[0] = '\0';
		}
	}
	pthread_mutex_unlock(&lock);
	// The audio thread is allowed to wait for LOCAL bytes at open; it must not
	// perform HTTP itself. Failure is reported by acquire/the GUI error timer.
	return true;
}

void subsonicstream_prefetch(const char *path) {
	pthread_mutex_lock(&lock);
	if (!subsonicstream_owns(path)) {
		for (int i = 0; i < 2; i++) {
			if (i != wanted_slot && !slots[i].leased) {
				streambridge_cancel(slots[i].bridge);
				slots[i].path[0] = '\0';
			}
		}
		pthread_mutex_unlock(&lock);
		return;
	}
	if (wanted_slot >= 0) {
		streambridge_state_t current;
		streambridge_state(slots[wanted_slot].bridge, &current);
		// Current music always wins bandwidth. Retry on the next GUI tick if
		// its reserve is low; don't start a second fetch during a starvation.
		if (!current.failed && !current.canceled && (current.eof || current.buffered >= STREAMBRIDGE_PRECACHE)) {
			choose_slot(path, true);
		}
	}
	pthread_mutex_unlock(&lock);
}

static void cancel_locked(void) {
	for (int i = 0; i < 2; i++) {
		streambridge_cancel(slots[i].bridge);
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	wanted_slot = -1;
	last_error[0] = '\0'; // deliberate Stop/service switch discards stale feedback
}

void subsonicstream_cancel(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	pthread_mutex_unlock(&lock);
}

void subsonicstream_reset(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	// IDs only mean something on the server/account that supplied this page.
	// No stale playlist may silently reuse those IDs on a newly saved server.
	free(queue);
	queue = NULL;
	queue_count = 0;
	pthread_mutex_unlock(&lock);
}

bool subsonicstream_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = false;
	for (int i = 0; i < 2; i++) {
		if (slots[i].path[0]) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			wanted |= !state.canceled && !state.failed && !state.eof;
		}
	}
	pthread_mutex_unlock(&lock);
	return wanted;
}

streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate) {
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	int slot = wanted_slot;
	streambridge_t *bridge = NULL;
	if (index >= 0 && slot >= 0 && !slots[slot].leased && strcmp(slots[slot].path, path) == 0) {
		slots[slot].leased = true;
		bridge = slots[slot].bridge;
		if (duration) {
			*duration = queue[index].duration;
		}
		if (bitrate) {
			*bitrate = queue_quality.format == SUBSONIC_FORMAT_RAW ? 0 : queue_quality.max_bitrate;
		}
	}
	pthread_mutex_unlock(&lock);
	return bridge;
}

void subsonicstream_release(streambridge_t *bridge) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 2; i++) {
		if (slots[i].bridge == bridge) {
			// Preserve a useful failure for the GUI even when teardown wins the
			// race with its next tick. Cancellation isn't a playback failure.
			streambridge_state_t state;
			streambridge_state(bridge, &state);
			if (state.failed && !state.canceled && !slots[i].error_reported) {
				snprintf(last_error, sizeof(last_error), "%s", state.error);
			}
			streambridge_cancel(bridge);
			slots[i].leased = false;
			slots[i].path[0] = '\0';
			// Do not clear wanted_slot here: EOF posts an advance which can
			// already have promoted the other slot before this lease closes.
			break;
		}
	}
	pthread_mutex_unlock(&lock);
}

bool subsonicstream_metadata(const char *path, song_metadata_t *out) {
	if (!subsonicstream_owns(path)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	if (index >= 0) {
		const subsonic_entry_t *track = &queue[index];
		snprintf(out->title, sizeof(out->title), "%s", track->title);
		snprintf(out->artist, sizeof(out->artist), "%s", track->artist);
		snprintf(out->album, sizeof(out->album), "%s", track->album);
		out->track_number = track->track_number;
		out->has_tags = true;
	}
	pthread_mutex_unlock(&lock);
	return true;
}

bool subsonicstream_take_error(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	if (!last_error[0] && wanted_slot >= 0) {
		slot_t *slot = &slots[wanted_slot];
		streambridge_state_t state;
		streambridge_state(slot->bridge, &state);
		if (state.failed && !state.canceled && !slot->error_reported) {
			snprintf(last_error, sizeof(last_error), "%s", state.error);
			slot->error_reported = true;
		}
	}
	bool have = last_error[0] != '\0';
	if (have) {
		snprintf(out, size, "%s", last_error);
		last_error[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
	return have;
}
