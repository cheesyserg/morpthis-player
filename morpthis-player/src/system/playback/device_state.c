#include "device_state.h"

#include "src/gui/apps/apphost.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/audiobook.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/radio.h"
#include "src/system/audio/replaygain.h"
#include "src/system/device/system.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Cache of the currently loaded track's metadata, refreshed whenever a new
// file is loaded via device_state_play_file(). Only ever touched from the
// LVGL/UI thread (same as the rest of player.c today), so no lock needed.
static song_metadata_t current_metadata;
static char current_metadata_file[512] = {0};

// ---------------------------------------------------------------------------
// "Remember track": where playback was, kept across a power cycle
//
// It lives here and not in the player screen for one reason: the two things
// that end a session -- the power menu and the automatic shutdown -- do not go
// anywhere near the interface, so a position noticed only by a UI poll would
// never be written by either of them.
//
// When it writes:
//
//   the file changed, or the state did (play, pause, stop) -- at once;
//
//   the position moved by more than REMEMBER_MOVE_SECS since the last write.
//   Without this a seek made while PAUSED is never written at all: nothing
//   about the state has changed and the ten-second timer only runs while
//   playing;
//
//   every REMEMBER_PERIOD_MS while playing, so a flat battery loses that much
//   and no more;
//
//   once when playback stops, so a queue that runs to its end does not come
//   back up to ten seconds short of where it finished.
//
// And the one thing it refuses to do: write a zero over a position it has just
// restored. A track brought back at boot goes through a moment where the
// decoder has opened the file and the seek has not landed yet, and the engine
// reports 0; a poll in that window would write 0 over the very position being
// restored. The latch below holds every write until the engine reports the
// position that was asked for, and lets go on its own after
// REMEMBER_RESTORE_GRACE_MS so a seek that never lands cannot silence the
// feature for the session.
// ---------------------------------------------------------------------------

#define REMEMBER_PERIOD_MS 10000
#define REMEMBER_MOVE_SECS 1.0
#define REMEMBER_RESTORE_GRACE_MS 8000

// Everything the decision needs to remember between calls. In a struct, and
// the decision itself a pure function of it, so a bench can walk a whole
// session through the real rules without a decoder or a database anywhere near
// it (tools/test_remember.c).
typedef struct {
	char file[512];
	audio_status_t status;
	double pos;
	uint32_t saved_ms;
	bool have;
	double restore_pos; // what the boot restore asked for, -1 for nothing pending
	uint32_t restore_ms;
} remember_state_t;

static remember_state_t remember = {.pos = -1.0, .restore_pos = -1.0};

// Whether this sample is worth writing. Updates `st` when it says yes, so the
// caller has nothing to remember on its own.
static bool remember_should_write(remember_state_t *st, const char *file, audio_status_t status, double pos,
								  uint32_t now) {
	// The restore latch: hold everything until the engine reports the position
	// that was asked for, and let go on its own if it never does.
	if (st->restore_pos >= 0) {
		bool arrived = pos >= st->restore_pos - REMEMBER_MOVE_SECS;
		bool waited = (uint32_t)(now - st->restore_ms) > REMEMBER_RESTORE_GRACE_MS;
		if (!arrived && !waited) {
			return false;
		}
		st->restore_pos = -1.0;
	}

	bool same_file = st->have && strncmp(st->file, file, sizeof(st->file)) == 0;
	double moved = pos - st->pos;
	if (moved < 0) {
		moved = -moved;
	}

	bool changed = !same_file || status != st->status;
	// A position that moved while nothing was PLAYING is a seek: nobody else
	// can have moved it. A drag of the slider with the track paused changes no
	// state, and the ten-second timer only runs while the music does, so this is
	// the only rule that catches it.
	//
	// While playing it deliberately does NOT apply: the position moves by
	// itself, and writing every time it has moved a second is a write to the
	// card every second.
	bool drifted = same_file && st->pos >= 0 && status != AUDIO_STATUS_PLAYING && moved >= REMEMBER_MOVE_SECS;
	bool due = status == AUDIO_STATUS_PLAYING && (uint32_t)(now - st->saved_ms) >= REMEMBER_PERIOD_MS;

	if (!changed && !drifted && !due) {
		return false;
	}

	snprintf(st->file, sizeof(st->file), "%s", file);
	st->status = status;
	st->pos = pos;
	st->saved_ms = now;
	st->have = true;
	return true;
}

// Milliseconds from an arbitrary origin. Only ever subtracted from itself, so
// where it starts does not matter.
static uint32_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool remember_enabled(void) { return config_get_int("player", "remember_track", 0) != 0; }

// A track this player must not offer to come back to: a live stream has no
// position, and a streaming cache file will not exist at the next boot.
static bool remember_worth_saving(const device_state_t *state) {
	return !state->live && state->current_file[0] && !qobuzcache_owns(state->current_file) &&
		   !tidalcache_owns(state->current_file);
}

// True when the current track was restored paused at boot and has not yet
// been started in this session.
static bool restored_uncounted;

// ---------------------------------------------------------------------------
// Work a track start notes instead of doing
//
// Starting a track runs on the interface thread and used to end with two card
// writes: the play count and the queue position. Each is a journal round-trip
// on an SD card, and together they are the longest part of a skip that the user
// feels -- the picture swap is thirty milliseconds, and these are the rest.
//
// Nothing waits for either. So they are noted here and written on a later pass
// of the progress poll, which runs on the same thread with nothing else to do:
// the write costs the same, but it lands where no frame is being drawn for a
// finger, and nobody sees it.
//
// Bounded, and the queue is coalesced -- twenty skips in two seconds are one
// queue write, because only the latest position is worth anything.
// ---------------------------------------------------------------------------
#define PENDING_PLAYS_MAX 8
#define BOOKKEEPING_DELAY_MS 400

typedef struct {
	char path[512];
	char name[256];
	char artist[256];
} pending_play_t;

static pending_play_t pending_plays[PENDING_PLAYS_MAX];
static int pending_play_count;
static bool queue_save_pending;
static uint32_t bookkeeping_since; // when the oldest unwritten item was noted

// The tags of a track the cache did not have, waiting to be written down. One
// slot: by the time the poll comes round the player is on the next track, so
// what is worth keeping is the one just read, and the scan will fill in the rest
// of the library anyway.
static bool pending_meta_valid;
static char pending_meta_path[512];
static song_metadata_t pending_meta;

// Starts the clock the delay is measured from, if nothing was already waiting:
// the point is to get past the frame the user's finger drew, not to hold every
// item for a fixed period of its own.
static void bookkeeping_note(void) {
	if (!queue_save_pending && pending_play_count == 0 && !pending_meta_valid) {
		bookkeeping_since = mono_ms();
	}
}

static void device_state_bookkeeping_flush(void);

// Counts the play later, or on the spot if the queue of them is full -- eight
// track starts inside the delay -- which is there to keep the array from
// growing rather than as a path anyone takes.
static void record_current_track_play(void) {
	if (!current_metadata_file[0] || audiobook_is_playing() || podcastcache_is_episode(current_metadata_file) ||
		qobuzcache_owns(current_metadata_file) || tidalcache_owns(current_metadata_file)) {
		return;
	}
	if (pending_play_count >= PENDING_PLAYS_MAX) {
		library_record_play(current_metadata_file, current_metadata.title, current_metadata.artist);
		return;
	}

	// Copied, not pointed at: by the time this is written down the player is
	// on a different track and current_metadata describes that one.
	pending_play_t *p = &pending_plays[pending_play_count++];
	snprintf(p->path, sizeof(p->path), "%s", current_metadata_file);
	snprintf(p->name, sizeof(p->name), "%s", current_metadata.title);
	snprintf(p->artist, sizeof(p->artist), "%s", current_metadata.artist);
	bookkeeping_note();
}

void device_state_remember_restored(const char *path, double position) {
	snprintf(remember.file, sizeof(remember.file), "%s", path ? path : "");
	remember.status = AUDIO_STATUS_PAUSED;
	remember.pos = position;
	remember.saved_ms = mono_ms();
	remember.have = true;
	remember.restore_pos = position;
	remember.restore_ms = remember.saved_ms;
	restored_uncounted = path && path[0];
}

void device_state_remember_note(void) {
	if (!remember_enabled()) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!remember_worth_saving(&state)) {
		return;
	}
	if (remember_should_write(&remember, state.current_file, state.status, state.progress_current_secs,
							  mono_ms())) {
		library_playback_state_save(state.current_file, state.progress_current_secs);
	}
}

// Where a book got to, written through on the way out: the power going off or
// the card going away. Not subject to "remember track", as the player's own
// saves are not. Nothing under a second, which is a book whose resume seek
// the engine has not reached yet rather than a place anyone stopped at.
static void flush_book_position(const device_state_t *state) {
	if (state->live || !audiobook_is_playing() || state->progress_current_secs <= 1.0) {
		return;
	}
	audiobook_note_position(state->progress_current_secs, state->progress_total_secs, true);
}

void device_state_remember_flush(void) {
	// Whatever the bookkeeping is still holding goes down before the power does.
	// The delay is for the interface's sake, not the card's.
	device_state_bookkeeping_flush();

	device_state_t state;
	device_state_get(&state);
	flush_book_position(&state);
	if (!remember_enabled()) {
		return;
	}
	if (!remember_worth_saving(&state)) {
		return;
	}
	// The latch still applies: on the way out, a position the engine has not
	// reached yet is still the one that was asked for, and writing the zero it
	// is currently reporting would throw it away at the last moment.
	if (remember.restore_pos >= 0 && state.progress_current_secs < remember.restore_pos - REMEMBER_MOVE_SECS) {
		return;
	}
	remember.restore_pos = -1.0;
	library_playback_state_save(state.current_file, state.progress_current_secs);
	snprintf(remember.file, sizeof(remember.file), "%s", state.current_file);
	remember.status = state.status;
	remember.pos = state.progress_current_secs;
	remember.saved_ms = mono_ms();
	remember.have = true;
}

void device_state_get(device_state_t *out) {
	if (!out)
		return;

	// A radio has the DAC through audio_external_*, which audio.c's own
	// status does not know about -- so the live source is asked first and
	// answers for the whole now-playing picture when it is on.
	out->live = false;
	out->live_cover[0] = '\0';

	if (radio_is_active()) {
		radio_now_t now;
		radio_get_now(&now);

		out->live = true;
		// Connecting counts as playing: it is on, and the button that would
		// stop it should say so. A station that has been stopped reads as
		// stopped, and its button goes back to play.
		out->status = (now.playing || now.connecting) ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED;
		out->current_file[0] = '\0';
		out->progress_current_secs = 0;
		out->progress_total_secs = 0;
		out->stream_sample_rate = now.sample_rate;
		out->stream_channels = now.channels;
		snprintf(out->live_cover, sizeof(out->live_cover), "%s", now.cover_path);

		memset(&out->metadata, 0, sizeof(out->metadata));
		snprintf(out->metadata.title, sizeof(out->metadata.title), "%s", now.station);

		// The second line: what the stream says is on air, or -- before the
		// first frame has come out of it -- what the player is busy doing.
		// Opening a connection to a radio server takes a moment, and a blank
		// line for that moment reads as a station that does not work.
		if (now.error[0]) {
			// Whatever went wrong goes where the on-air title would be: it is
			// the line the user is already reading, and a station that simply
			// sits there silent is the worst way to say "this one does not
			// work here".
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", now.error);
		} else if (now.title[0]) {
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", now.title);
		} else if (now.connecting) {
			snprintf(out->metadata.artist, sizeof(out->metadata.artist), "%s", tr("connecting"));
		}
		out->metadata.has_tags = true;
	} else {
		out->status = audio_get_status();
		audio_get_current_file(out->current_file, sizeof(out->current_file));
		audio_get_progress(&out->progress_current_secs, &out->progress_total_secs);

		// While a skip button is held the position shown is the one the thumb
		// has run to, not the one the decoder is at: the seek itself happens
		// when the button comes up (see the scrub section below).
		double scrub = 0;
		if (out->progress_total_secs > 0 && device_state_scrub_active(&scrub)) {
			out->progress_current_secs = scrub;
		}
		audio_get_stream_info(&out->stream_sample_rate, &out->stream_channels);

		// Starting a radio stops local playback, which leaves audio.c with no
		// file at all -- so when the radio is switched off the player would
		// come back to a blank screen instead of to the track it was on.
		// The loaded track is still known here; report it, stopped. Pressing
		// play then restarts it, which is what the button already does from a
		// stopped state.
		if (!out->current_file[0] && current_metadata_file[0]) {
			snprintf(out->current_file, sizeof(out->current_file), "%s", current_metadata_file);
		}

		out->metadata = current_metadata;
	}

	char *battery = read_battery_percent();
	strncpy(out->battery_percent, battery ? battery : "!!", sizeof(out->battery_percent) - 1);
	out->battery_percent[sizeof(out->battery_percent) - 1] = '\0';
	out->battery_charging = read_battery_charging();

	out->volume = get_volume_percent();
}

const song_metadata_t *device_state_loaded_metadata(const char **file_out) {
	if (file_out) {
		*file_out = current_metadata_file;
	}
	return &current_metadata;
}

static void playlist_resync_to_current(void);

// ---------------------------------------------------------------------------
// Mirroring the queue on disk
//
// Without this a power cycle leaves the player with the remembered track and no
// queue around it, so the first next/prev lands back on the same song. The
// queue is written out with every track change, but the list itself only when
// it actually changed (playlist_revision): rewriting a few thousand rows on a
// class-10 card for every track would be felt.
// ---------------------------------------------------------------------------

static unsigned saved_queue_revision;
static bool queue_ever_saved;

// Writes a library-backed queue down as its query. The position is the entry as
// stored, not where it sits in the order being played: a shuffled queue is
// dealt again when it comes back, and the old deal's position would land on an
// unrelated track. Anything "add to queue" put there afterwards is not in the
// query and goes along beside it.
static void queue_save_query_form(const library_index_spec_t *spec) {
	int extra_count = playlist_appended_count();
	char **extra = NULL;
	int *slots = NULL;
	int filled = 0;

	if (extra_count > 0) {
		extra = calloc((size_t)extra_count, sizeof(*extra));
		slots = calloc((size_t)extra_count, sizeof(*slots));
		if (extra && slots) {
			// Where each one sits, taken before the loop: one walk of the order
			// for all of them rather than a search each.
			playlist_appended_slots(slots, extra_count);
			for (int i = 0; i < extra_count; i++) {
				char path[512];
				if (!playlist_appended_at(i, path, sizeof(path))) {
					continue;
				}
				extra[filled] = strdup(path);
				if (!extra[filled]) {
					break;
				}
				slots[filled] = slots[i]; // a skipped entry must not leave its slot behind
				filled++;
			}
		}
	}

	library_queue_save_query(spec, (int)playlist_current_entry(), (const char *const *)extra, slots, filled);

	for (int i = 0; i < filled; i++) {
		free(extra[i]);
	}
	free(extra);
	free(slots);
}

// The queue as it stands, written down. Called only from the bookkeeping below,
// which is what decides when.
static void queue_save_now(void) {
	int count = playlist_count();
	int current = playlist_current_index();
	if (count <= 0 || current < 0) {
		return;
	}

	unsigned revision = playlist_revision();
	if (queue_ever_saved && revision == saved_queue_revision) {
		// Only the position moved. Which of the two forms holds the queue
		// decides where that goes.
		library_index_spec_t moved;
		if (playlist_library_spec(&moved)) {
			queue_save_query_form(&moved);
		} else {
			library_queue_save_index(current);
		}
		return;
	}

	// A queue that is a library list is written down as the query behind it.
	// The rows would be the same list at ten thousand times the cost, and on a
	// large library the write alone would outlast the track.
	library_index_spec_t spec;
	if (playlist_library_spec(&spec)) {
		queue_save_query_form(&spec);
		saved_queue_revision = revision;
		queue_ever_saved = true;
		return;
	}

	char **list = calloc((size_t)count, sizeof(*list));
	if (!list) {
		return;
	}

	int filled = 0;
	bool from_stream = false;
	for (int i = 0; i < count; i++) {
		char path[512];
		if (!playlist_path_at(i, path, sizeof(path))) {
			continue;
		}
		// A streaming queue is not saved: the files are transient (the caches
		// empty themselves) and the next boot would restore a queue of paths
		// that no longer exist, mixed in with the user's own tracks. The RAM
		// bridge's source IDs are transient for the same reason.
		if (qobuzcache_owns(path) || tidalcache_owns(path)) {
			from_stream = true;
			break;
		}
		list[filled] = strdup(path);
		if (!list[filled]) {
			break;
		}
		filled++;
	}

	if (filled > 0 && !from_stream) {
		library_queue_save((const char *const *)list, filled, current, playlist_is_custom());
		saved_queue_revision = revision;
		queue_ever_saved = true;
	}

	for (int i = 0; i < filled; i++) {
		free(list[i]);
	}
	free(list);
}

// The queue is written later for the same reason the play count is. What the
// flush records is the position the queue has by then, which is the only one
// worth having.
static void queue_persist(void) {
	queue_save_pending = true;
	bookkeeping_note();
}

// Writes everything the bookkeeping above is holding. Safe to call at any time:
// with nothing pending it does nothing at all.
static void device_state_bookkeeping_flush(void) {
	for (int i = 0; i < pending_play_count; i++) {
		library_record_play(pending_plays[i].path, pending_plays[i].name, pending_plays[i].artist);
	}
	pending_play_count = 0;

	if (pending_meta_valid) {
		pending_meta_valid = false;
		metadata_cache_store(pending_meta_path, &pending_meta);
	}

	if (queue_save_pending) {
		queue_save_pending = false;
		queue_save_now();
	}
}

void device_state_bookkeeping_tick(void) {
	if (!queue_save_pending && pending_play_count == 0 && !pending_meta_valid) {
		return;
	}
	if ((uint32_t)(mono_ms() - bookkeeping_since) < BOOKKEEPING_DELAY_MS) {
		return;
	}
	device_state_bookkeeping_flush();
}

// Not deferred. This is the one caller that is an edit to the mirror on disk
// rather than a consequence of the track moving, and the comment on its
// declaration says why a power-off straight after it must not lose it. Any
// write a track start was holding is settled here too, so the queue does not go
// down twice.
void device_state_queue_changed(void) {
	queue_save_pending = false;
	queue_save_now();
}

// Four streaming preparations: Qobuz, Tidal, podcasts and Subsonic.
#define PREPARE_CB_MAX 4
static device_state_prepare_cb prepare_cbs[PREPARE_CB_MAX];
static int prepare_cb_count;

void device_state_add_prepare_cb(device_state_prepare_cb cb) {
	if (!cb || prepare_cb_count >= PREPARE_CB_MAX) {
		return;
	}
	// Registering twice would mean preparing the same track twice. The cost is
	// one walk over four entries, once at start-up.
	for (int i = 0; i < prepare_cb_count; i++) {
		if (prepare_cbs[i] == cb) {
			return;
		}
	}
	prepare_cbs[prepare_cb_count++] = cb;
}

// Defined with the rest of the scrub state further down.
static void scrub_cancel(void);

// Where playback was when the storage under it went away: the card pulled out,
// or the card exported to a computer. Both stop the engine, and from the
// outside a stop is a stop -- pressing play afterwards started the track again
// from zero, whatever it had been paused at. See device_state_note_storage_gone().
static char interrupted_file[512];
static double interrupted_pos = -1.0;

void device_state_note_storage_gone(void) {
	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	device_state_t state;
	device_state_get(&state);
	// The databases on the card close right after this.
	flush_book_position(&state);
	// A second in is not a position worth coming back to, and a live stream has
	// none at all.
	if (state.live || !state.current_file[0] || state.progress_current_secs <= 1.0) {
		return;
	}

	snprintf(interrupted_file, sizeof(interrupted_file), "%s", state.current_file);
	interrupted_pos = state.progress_current_secs;
}

// A book plays at the audiobook speed, an episode at the podcast speed, and
// anything else at 1.0. Set at every track change and not only from the
// pop-over, so a song that follows a book at 1.5x is not also played at 1.5x.
static void apply_track_speed(void) {
	if (audiobook_is_playing()) {
		audio_set_speed(audiobook_speed());
	} else if (podcastcache_is_episode(current_metadata_file)) {
		audio_set_speed(podcast_speed());
	} else {
		audio_set_speed(1.0);
	}
}

// Loads metadata for `filepath` and starts playback, without touching the
// folder queue. Shared by fresh selections, queue advances, and replays.
//
// `position` is where to start; below zero means the beginning, which is what
// every caller but the resume below wants.
static void load_and_play_at(const char *filepath, double position) {
	// Every route by which a track starts -- a tap, next, previous, the queue
	// advancing on its own when one ends -- comes through here, and every line of
	// it runs on the interface thread. Reading the tags off the card, working out
	// the ReplayGain they ask for, recording the play in the library and writing
	// the queue back are each a card round trip, and the user sees the whole lot
	// as a stall between two frames. Reported only past the point where a frame
	// would have been late, with the tag read broken out: that is the part that
	// touches the file, so it is the part that moves first.
	uint32_t started = mono_ms();
	uint32_t tags_ms = 0;
	uint32_t record_ms = 0;

	// A position picked on the outgoing track means nothing on this one, and
	// the track can change under a held button -- the queue advances on its
	// own when the outgoing one ends.
	scrub_cancel();

	// A file and a stream cannot both have the output. Starting a track ends
	// the radio outright -- not merely stops it -- because from here on the
	// player belongs to the track. The same applies to an app left playing in
	// the background: its raw output is closed before the local decoder opens
	// the DAC, or the open would sit in the busy-retry loop and fail.
	apphost_release_audio();
	radio_clear();

	// Callers may replay the already-loaded track by passing current_metadata_file
	// itself, so guard against copying a buffer onto itself.
	if (filepath != current_metadata_file) {
		strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
		current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	}

	// The tags come from the card, not from the file: the scan parses every track
	// once and leaves the result in the artwork's database, which is a stat and
	// one row against the four or five file accesses metadata_read() makes. A
	// miss reads the file as before and hands the result to the bookkeeping, so
	// the second play of that track costs the row.
	if (!metadata_cache_load(current_metadata_file, &current_metadata)) {
		metadata_read(current_metadata_file, &current_metadata);
		pending_meta = current_metadata;
		snprintf(pending_meta_path, sizeof(pending_meta_path), "%s", current_metadata_file);
		pending_meta_valid = true;
		bookkeeping_note();
	}
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();
	tags_ms = mono_ms() - started;

	// The file may not be there yet: with Qobuz and Tidal the queue holds the
	// whole album and playback can reach a track before its download does. The
	// registered callback takes over and restarts this once the file is ready;
	// stop here rather than hand the decoder a path to nothing.
	for (int i = 0; i < prepare_cb_count; i++) {
		if (!prepare_cbs[i](current_metadata_file)) {
			queue_persist();
			return;
		}
	}

	if (position > 0) {
		audio_play_at(current_metadata_file, position);
	} else {
		audio_play(current_metadata_file);
		uint32_t t = mono_ms();
		record_current_track_play();
		record_ms = mono_ms() - t;
	}
	restored_uncounted = false;

	// Any track load supersedes the note: it belongs to the one press that
	// follows the storage coming back.
	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	queue_persist();

	uint32_t took = mono_ms() - started;
	if (took >= 20) {
		fprintf(stderr, "playback: starting a track took %u ms (tags %u, record %u)\n", took, tags_ms, record_ms);
	}
}

static void load_and_play(const char *filepath) { load_and_play_at(filepath, -1.0); }

// The same without starting: the track comes up paused at its beginning, and
// play starts it. The queue is left as it is.
static void load_paused(const char *filepath) {
	scrub_cancel();
	radio_clear();

	if (filepath != current_metadata_file) {
		snprintf(current_metadata_file, sizeof(current_metadata_file), "%s", filepath);
	}
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_paused(current_metadata_file, 0);
	audiobook_suppress_rewind_once();

	interrupted_file[0] = '\0';
	interrupted_pos = -1.0;

	queue_persist();
}

// The boot-time "remember track" restore: the same folder queue and metadata a
// tap on the file would build, but the track comes up paused at `position`
// instead of playing.
void device_state_restore_file(const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);

	strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_paused(current_metadata_file, position);
	device_state_remember_restored(current_metadata_file, position);

	queue_persist();
}

// The record the restored queue is, put back where playlist_clear() wiped it.
// Both restores go through here: without it the queue comes back and the label
// does not, and "play albums back to back" stays dead until a record is started
// by hand.
static void restore_queue_album(void) {
	char album[256];
	if (library_queue_load_album(album, (int)sizeof(album))) {
		playlist_set_album(album);
		char shown[sizeof(album)];
		library_album_title(album, shown, sizeof(shown));
		fprintf(stderr, "queue: the restored queue is the record \"%s\"\n", shown);
	}
	playlist_note_album_restored();
}

// The same restore, but with the queue that was saved next to the track, so
// next and prev work from the first press.
void device_state_restore_index(library_index_t *ix, int start_index, const char *const *extra,
								const int *extra_slots, int extra_count, const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	if (!playlist_load_index(ix, start_index)) {
		device_state_restore_file(filepath, position);
		return;
	}

	// Whatever "add to queue" had put into the list. Restored before the track
	// is located, so a remembered position inside one of them still resolves.
	//
	// Each goes back to the slot it was written down at. playlist_insert_next()
	// cannot be used for that: it can only insert after the current track, which
	// would lift tracks played hours ago back in front of playback. They arrive
	// already sorted by slot, which is what makes inserting them one after
	// another land each in the right place -- every insert shifts the rest up by
	// one.
	for (int i = 0; i < extra_count; i++) {
		if (!extra || !extra[i] || !extra[i][0]) {
			continue;
		}
		int slot = extra_slots ? extra_slots[i] : -1;
		if (slot < 0) {
			playlist_insert_next(extra[i]); // saved before the slot was written down
		} else {
			playlist_insert_at(extra[i], slot);
		}
	}

	// Prefer the remembered track over the saved position: the two agree unless
	// the library was rescanned since, and then the track is what the user last
	// saw while the position is a number about a list that has moved.
	//
	// When it cannot be found the saved offset goes with it: seeking an
	// unrelated track to three minutes in is worse than starting one at the top.
	if (filepath && filepath[0] && !playlist_locate(filepath)) {
		position = 0;
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		device_state_restore_file(filepath, position);
		return;
	}

	strncpy(current_metadata_file, path, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	replaygain_load(&current_metadata);
	apply_track_speed();

	// The queue just loaded is the saved one: do not write it straight back.
	saved_queue_revision = playlist_revision();
	queue_ever_saved = true;
	restore_queue_album();

	audio_play_paused(current_metadata_file, position);
	device_state_remember_restored(current_metadata_file, position);
}

void device_state_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
							   double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	if (!list || count <= 0) {
		device_state_restore_file(filepath, position);
		return;
	}

	playlist_load_paths(list, count, start_index);
	// playlist_load_paths() always marks the queue custom; a queue that was a
	// folder queue before the reboot has to stay one, or it would never be
	// refreshed from the disk again.
	playlist_set_custom(custom);

	// Prefer the remembered track over the saved index: the two agree unless
	// the position row was written after the list, and the track is what the
	// user last saw.
	if (filepath && filepath[0]) {
		playlist_locate(filepath);
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		device_state_restore_file(filepath, position);
		return;
	}

	strncpy(current_metadata_file, path, sizeof(current_metadata_file) - 1);
	current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	metadata_read(current_metadata_file, &current_metadata);
	// Whether the loaded file is an audiobook is decided here, once per track
	// change, and everything that dresses the player for one reads the answer
	// from it. Every route by which the track can change passes through one of
	// these calls.
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	// The queue just loaded is the saved one: do not write it straight back.
	saved_queue_revision = playlist_revision();
	queue_ever_saved = true;
	restore_queue_album();

	audio_play_paused(current_metadata_file, position);
	device_state_remember_restored(current_metadata_file, position);
}

// The same, but starting somewhere other than the beginning: an audiobook
// resumed where it was left. Split out rather than folded into a flag on
// load_and_play() because only a book has a position to come back to, and the
// difference should be visible at the call site.
void device_state_play_file_at(const char *filepath, double position) {
	// Same reason as load_and_play: this changes the track too.
	scrub_cancel();

	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);

	radio_clear();
	if (filepath != current_metadata_file) {
		strncpy(current_metadata_file, filepath, sizeof(current_metadata_file) - 1);
		current_metadata_file[sizeof(current_metadata_file) - 1] = '\0';
	}
	metadata_read(current_metadata_file, &current_metadata);
	audiobook_track_changed(current_metadata_file);
	// The ReplayGain the tags asked for, worked out once per track.
	replaygain_load(&current_metadata);
	apply_track_speed();

	audio_play_at(current_metadata_file, position);

	queue_persist();
}

void device_state_play_file(const char *filepath) {
	// The queue is built from the selected file's directory, so playback can
	// continue past this track.
	char folder[512];
	strncpy(folder, filepath, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = filepath;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';		   // terminate the folder portion
		filename = slash + 1;  // the bare file name within the folder
	}

	playlist_load_folder(folder, filename);

	load_and_play(filepath);
}

// Starts playback from an explicit list of paths: the queue becomes exactly
// that list, so next/prev walk it. What the library index pages call.
bool device_state_play_index(library_index_t *ix, int start_index) {
	if (!playlist_load_index(ix, start_index)) {
		return false;
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false;
	}
	load_and_play(path);
	return true;
}

void device_state_play_list(const char *const *list, int count, int start_index) {
	if (!list || count <= 0) {
		return;
	}
	playlist_load_paths(list, count, start_index);

	char path[512];
	if (playlist_current_path(path, sizeof(path))) {
		load_and_play(path);
	}
}

// The same for a queue that already carries its own order: a podcast's
// episodes. See playlist_load_paths_ordered().
void device_state_play_list_ordered(const char *const *list, int count, int start_index) { device_state_play_list_ordered_at(list, count, start_index, -1.0); }

void device_state_play_list_ordered_at(const char *const *list, int count, int start_index, double position) {
	if (!list || count <= 0) {
		return;
	}
	playlist_load_paths_ordered(list, count, start_index);

	char path[512];
	if (playlist_current_path(path, sizeof(path))) {
		load_and_play_at(path, position);
	}
}

// Jumps to a specific entry of the current queue: the queue page's tap.
bool device_state_play_queue_index(int index) {
	if (!playlist_set_current(index)) {
		return false;
	}
	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false;
	}
	load_and_play(path);
	return true;
}

// ---------------------------------------------------------------------------
// Consecutive album playback
//
// Normally a queue that runs out simply stops. With this on, the next album
// starts instead: the next one alphabetically, wrapping round at Z, which is
// the order the albums page lists them in, so what follows is where the finger
// would have gone anyway.
//
// It only fires on a queue that is an album (see playlist_album): a folder
// queue or a hand-made list running out is not a record ending.
// ---------------------------------------------------------------------------

bool device_state_album_chaining(void) { return config_get_int("player", "album_chaining", 0) != 0; }

void device_state_set_album_chaining(bool on) {
	config_set_int("player", "album_chaining", on ? 1 : 0);
	config_save();
}

bool device_state_folder_chaining(void) { return config_get_int("player", "folder_chaining", 0) != 0; }

void device_state_set_folder_chaining(bool on) {
	config_set_int("player", "folder_chaining", on ? 1 : 0);
	config_save();
}

// One pass over the records, keeping both neighbours of the current one. It
// cannot stop at the next any more: going backwards needs the last record on
// the card, which is only known once the walk has run out.
typedef struct {
	const char *current;
	char first[256]; // for the wrap round at the end of the alphabet
	char last[256];	 // ... and at the start of it
	char prev[256];
	char next[256];
	bool seen_current;
	bool found_next;
	bool found_prev;
} album_walk_t;

static bool album_walk_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)path;
	(void)artist;
	album_walk_t *w = user;

	if (!w->first[0]) {
		snprintf(w->first, sizeof(w->first), "%s", name);
	}

	if (w->seen_current && !w->found_next) {
		snprintf(w->next, sizeof(w->next), "%s", name);
		w->found_next = true;
	} else if (!w->seen_current && library_album_same(name, w->current)) {
		// Whatever came immediately before is the previous record; nothing
		// before it means the current one is the first, and the wrap uses
		// `last` instead.
		if (w->last[0]) {
			snprintf(w->prev, sizeof(w->prev), "%s", w->last);
			w->found_prev = true;
		}
	}

	if (library_album_same(name, w->current)) {
		w->seen_current = true;
	}
	snprintf(w->last, sizeof(w->last), "%s", name);
	return true;
}

typedef struct {
	char **paths;
	int count;
	int capacity;
} album_tracks_t;

static bool album_track_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	album_tracks_t *t = user;
	if (!path || !path[0]) {
		return true;
	}
	if (t->count >= t->capacity) {
		int want = t->capacity ? t->capacity * 2 : 32;
		char **grown = realloc(t->paths, (size_t)want * sizeof(*grown));
		if (!grown) {
			return false;
		}
		t->paths = grown;
		t->capacity = want;
	}
	t->paths[t->count] = strdup(path);
	if (!t->paths[t->count]) {
		return false;
	}
	t->count++;
	return true;
}

// Starts the record either side of `album`. Going forward it opens on the first
// track, going back on the last: "previous" from the top of a record means the
// end of the one before it, the same way it means the end of the track before
// inside one. False when there is no library, no record that side, or the one
// found is empty.
static bool play_album_beside(const char *album, bool forward) {
	album_walk_t walk = {album, "", "", "", "", false, false, false};
	library_for_each(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, album_walk_cb, &walk);

	const char *wanted = NULL;
	if (forward) {
		if (walk.found_next) {
			wanted = walk.next;
		} else if (walk.first[0] && !library_album_same(walk.first, album)) {
			wanted = walk.first; // the current record was the last: round to the top
		}
	} else {
		if (walk.found_prev) {
			wanted = walk.prev;
		} else if (walk.last[0] && !library_album_same(walk.last, album)) {
			wanted = walk.last; // the current record was the first: round to the bottom
		}
	}
	if (!wanted || !wanted[0]) {
		return false;
	}

	album_tracks_t tracks = {NULL, 0, 0};
	library_for_each_ordered(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, wanted, LIBRARY_ORDER_DEFAULT,
							 album_track_cb, &tracks);

	bool started = false;
	if (tracks.count > 0) {
		device_state_play_list((const char *const *)tracks.paths, tracks.count,
							   forward ? 0 : tracks.count - 1);
		playlist_set_album(wanted); // so the record beyond that one can follow too
		char shown[256];
		library_album_title(wanted, shown, sizeof(shown));
		fprintf(stderr, "album: %s \"%s\" (%d tracks)\n", forward ? "on to" : "back to", shown, tracks.count);
		started = true;
	}

	for (int i = 0; i < tracks.count; i++) {
		free(tracks.paths[i]);
	}
	free(tracks.paths);
	return started;
}

// Starts the folder either side of the one the queue was built from, opening on
// its first track going forward and on its last going back. False when there is
// nothing that side -- a queue that is not a folder's, a folder outside the
// card, or a card whose only music is right here.
static bool play_folder_beside(const char *file, bool forward) {
	const char *root = playlist_card_root();
	if (!root || !root[0] || !file || !file[0]) {
		return false;
	}

	char folder[512];
	snprintf(folder, sizeof(folder), "%s", file);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';

	char wanted[512];
	bool have = forward ? playlist_next_folder(root, folder, wanted, sizeof(wanted))
						: playlist_prev_folder(root, folder, wanted, sizeof(wanted));
	if (!have) {
		return false;
	}

	// A folder queue and not a list: the option only means anything if the
	// folder after this one can be followed by the one after that.
	playlist_load_folder(wanted, NULL);

	if (!forward) {
		int count = playlist_count();
		if (count > 0) {
			playlist_set_current(count - 1);
		}
	}

	char path[512];
	if (!playlist_current_path(path, sizeof(path))) {
		return false; // it looked like it held a track and does not any more
	}
	load_and_play(path);
	fprintf(stderr, "folder: %s \"%s\"\n", forward ? "on to" : "back to", wanted);
	return true;
}

// Either option, whichever applies: the record beside this one if the queue is
// a record and records are chained, otherwise the folder beside it if the queue
// is a folder and folders are chained. The one place both rules live, so that
// the prev and next keys and a track running out all take the same step.
//
// Says nothing about WHEN to take it: the caller decides that, and the two
// callers differ. A track running out has to ignore repeat-one, which is an
// instruction to stay put; a key pressed on purpose is not the end of a queue
// and does not.
static bool chain_beside(bool forward) {
	const char *album = playlist_album();
	if (album && album[0] && device_state_album_chaining()) {
		char held[256];
		snprintf(held, sizeof(held), "%s", album); // the queue is about to be replaced
		if (play_album_beside(held, forward)) {
			return true;
		}
	}
	if (device_state_folder_chaining() && !playlist_is_custom() &&
		play_folder_beside(current_metadata_file, forward)) {
		return true;
	}
	return false;
}

bool device_state_advance_auto(char *out_path, size_t out_size) {
	if (radio_is_active()) {
		return false; // a stream never finishes, so there is nothing to advance to
	}

	// A book ends where it ends. The queue behind a book in one file is its
	// folder, and on a card that folder is a shelf of other books -- rolling
	// straight into somebody else's novel at the end of this one is not what
	// finishing a book should do. (The position was already cleared by then,
	// so opening it again starts it over.)
	//
	// A folder book goes on into its next file: its queue is its parts. With
	// "stop at end of chapter" on, that file is loaded paused at its start,
	// since for a book of this shape a file is a chapter.
	if (audiobook_is_playing()) {
		if (!audiobook_has_next_part()) {
			return false;
		}
		char next[512];
		if (!playlist_advance_auto(next, sizeof(next))) {
			return false;
		}
		if (audiobook_stop_at_chapter_end()) {
			load_paused(next);
		} else {
			load_and_play(next);
		}
		if (out_path && out_size > 0) {
			snprintf(out_path, out_size, "%s", next);
		}
		return true;
	}

	char path[512];
	playlist_resync_to_current();

	// The end of a record or of a folder, asked before the queue is advanced
	// rather than after it fails to advance.
	//
	// Asked afterwards it would almost never fire: only PLAYBACK_MODE_NORMAL
	// runs a queue out, and repeat-all wraps back to the first track instead, so
	// the album would play through and start again -- exactly what the option is
	// meant to stop. Repeat-one still wins here: it is an explicit instruction
	// to stay on this track, not a queue that reached its end.
	if (playlist_at_end() && playlist_get_mode() != PLAYBACK_MODE_REPEAT_ONE && chain_beside(true)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	if (!playlist_advance_auto(path, sizeof(path))) {
		return false; // the queue is spent, and there was no record to follow
	}

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

// Re-anchors the queue to the track that is actually loaded. If the two ever
// drift apart -- whatever the cause -- a "next" from a stale index replays a
// track the user already heard, so the first press restarts the same song and
// only the second moves on. Cheap to check, and it makes every manual advance
// immune to the drift instead of hunting each source of it.
static void playlist_resync_to_current(void) {
	if (!current_metadata_file[0]) {
		return;
	}

	char queue_now[512];
	if (playlist_current_path(queue_now, sizeof(queue_now)) && strcmp(queue_now, current_metadata_file) == 0) {
		return; // in agreement, nothing to do
	}

	// A custom queue (a library list, the favourites) must never be replaced by
	// the folder of whatever is playing: that would turn "next within all
	// tracks" back into "next within the album". Point it at the loaded track
	// instead, if it is in there.
	if (playlist_is_custom()) {
		playlist_locate(current_metadata_file);
		return;
	}

	char folder[512];
	strncpy(folder, current_metadata_file, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = '\0';

	const char *filename = current_metadata_file;
	char *slash = strrchr(folder, '/');
	if (slash) {
		*slash = '\0';
		filename = slash + 1;
	}

	playlist_load_folder(folder, filename);
}

bool device_state_next(char *out_path, size_t out_size) {
	char path[512];
	playlist_resync_to_current();

	// The same step a track running out would take. Without this the two
	// options only worked when the player was left alone: pressing next on the
	// last track of a record went round to its first instead of moving on to
	// the record after, which is the opposite of what "play albums back to
	// back" says.
	if (playlist_at_end() && chain_beside(true)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	if (!playlist_next(path, sizeof(path)))
		return false;

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

bool device_state_prev(char *out_path, size_t out_size) {
	char path[512];
	playlist_resync_to_current();

	// And the mirror of it: from the first track of a record, back is the END
	// of the record before, not the end of this one. Nothing automatic ever
	// goes this way, so this direction exists only for the key.
	if (playlist_current_index() <= 0 && chain_beside(false)) {
		if (out_path && out_size > 0) {
			playlist_current_path(out_path, out_size);
		}
		return true;
	}

	if (!playlist_prev(path, sizeof(path)))
		return false;

	load_and_play(path);

	if (out_path && out_size > 0) {
		strncpy(out_path, path, out_size - 1);
		out_path[out_size - 1] = '\0';
	}
	return true;
}

bool device_state_take_completion(void) { return audio_take_completion(); }

audio_status_t device_state_toggle_play_pause(void) {
	// A live stream cannot be paused: there is no position to come back to,
	// and starting again means reconnecting. Whoever pressed play/pause --
	// the player's button, the control centre, the physical key -- means
	// "stop this". Without this, the paused track underneath would resume on
	// top of the stream and both would be writing to one output.
	if (radio_is_active()) {
		// While it is still connecting the key does nothing: stopping a
		// connection halfway only makes the wait for the next one longer.
		if (radio_is_connecting()) {
			return AUDIO_STATUS_PLAYING;
		}
		if (radio_is_playing()) {
			radio_stop();
			return AUDIO_STATUS_STOPPED;
		}
		// Stopped on a station: play means reconnect to it, not go back to
		// whatever track was loaded before.
		radio_resume();
		return AUDIO_STATUS_PLAYING;
	}

	switch (audio_get_status()) {
	case AUDIO_STATUS_PLAYING:
		audio_pause();
		return AUDIO_STATUS_PAUSED;
	case AUDIO_STATUS_PAUSED:
		// Rewind after pause: on a book, resuming winds back a few seconds,
		// because whatever was being said when the pause landed was almost
		// certainly mid-sentence. Done here rather than in the player, so the
		// side key and the control centre get it too.
		if (audiobook_is_playing() && audiobook_rewind_enabled() && !audiobook_take_rewind_suppression()) {
			double current = 0, total = 0;
			audio_get_progress(&current, &total);
			double back = current - (double)audiobook_rewind_seconds();
			audio_seek(back > 0 ? back : 0);
		}
		audio_resume();
		if (restored_uncounted) {
			restored_uncounted = false;
			record_current_track_play();
		}
		return AUDIO_STATUS_PLAYING;
	case AUDIO_STATUS_STOPPED:
	default:
		// Playback ended (or was stopped): restart the currently-loaded track
		// from the beginning so pressing play always plays the shown song --
		// unless the engine was stopped by the storage going away rather than
		// by the track ending, in which case play means carry on from where the
		// card was pulled.
		if (current_metadata_file[0]) {
			double resume = -1.0;
			if (interrupted_pos > 0 && strcmp(interrupted_file, current_metadata_file) == 0) {
				resume = interrupted_pos;
			}
			load_and_play_at(current_metadata_file, resume);
			return AUDIO_STATUS_PLAYING;
		}
		return AUDIO_STATUS_STOPPED;
	}
}

// ---------------------------------------------------------------------------
// Scrubbing: holding a skip button to run through the track
//
// A held button asks for a new position four times a second, and a real seek is
// not a cheap thing to ask for four times a second. Every one of them drops
// what the sound card has queued and prepares the stream again, and on this
// hardware the stream only starts once its buffer is full -- about three
// quarters of a second of audio. Emptying it every quarter second means it
// never gets there: nothing comes out while the button is held, and the DAC is
// left being reprogrammed under a stream that never ran, which ends playback
// altogether.
//
// So a hold moves a number, not the decoder. The position the interface draws
// follows the thumb, the audio thread is told nothing, and the one real seek
// happens when the button comes up -- which is also what it looks like from the
// outside: the bar runs to where it is wanted and the music starts there.
// ---------------------------------------------------------------------------
static bool scrubbing;
static double scrub_target;
// The track the open scrub was measured on. A hold that runs to the end of a
// track lets the queue move on underneath it, and seconds counted on the track
// that finished mean nothing on the one that started.
static char scrub_file[512];

// Guards the three above: they are written from the input thread and read from
// the interface's. Nothing else in this file is shared like that, so the lock
// is theirs alone.
static pthread_mutex_t scrub_lock = PTHREAD_MUTEX_INITIALIZER;

static void scrub_cancel(void) {
	pthread_mutex_lock(&scrub_lock);
	scrubbing = false;
	pthread_mutex_unlock(&scrub_lock);
}

bool device_state_scrub_active(double *position_out) {
	pthread_mutex_lock(&scrub_lock);
	bool active = scrubbing;
	if (active && position_out) {
		*position_out = scrub_target;
	}
	pthread_mutex_unlock(&scrub_lock);
	return active;
}

void device_state_scrub_by(double delta) {
	double current = 0;
	double total = 0;
	audio_get_progress(&current, &total);
	if (total <= 0) {
		return; // nothing with a length to run through: a live stream
	}

	pthread_mutex_lock(&scrub_lock);
	if (!scrubbing) {
		scrubbing = true;
		scrub_target = current; // from where the music actually is
		snprintf(scrub_file, sizeof(scrub_file), "%s", current_metadata_file);
	}
	scrub_target += delta;
	if (scrub_target < 0) {
		scrub_target = 0;
	}
	// A whisker short of the end, so running off the edge does not finish the
	// track and start the next one.
	if (scrub_target > total - 1) {
		scrub_target = total - 1;
		if (scrub_target < 0) {
			scrub_target = 0;
		}
	}
	pthread_mutex_unlock(&scrub_lock);
}

void device_state_scrub_commit(void) {
	pthread_mutex_lock(&scrub_lock);
	bool was = scrubbing;
	double target = scrub_target;
	char file[sizeof(scrub_file)];
	snprintf(file, sizeof(file), "%s", scrub_file);
	scrubbing = false;
	pthread_mutex_unlock(&scrub_lock);

	// The track can end while the button is held, and the button comes up
	// after the queue has already started the next one: seek only the track
	// the position was counted on.
	if (was && strcmp(file, current_metadata_file) == 0) {
		device_state_seek(target);
	}
}

void device_state_seek(double seconds) {
	// A seek from anywhere else -- the slider, the queue -- ends a scrub that
	// was still open, or the interface would go on drawing a thumb that no
	// longer means anything.
	scrub_cancel();

	// If playback already ended, restart the current track first so seeking
	// (e.g. dragging the slider or hitting prev) resumes playback intuitively
	// instead of silently doing nothing.
	if (audio_get_status() == AUDIO_STATUS_STOPPED && current_metadata_file[0]) {
		load_and_play(current_metadata_file);
	}
	audio_seek(seconds);
}

void device_state_stop(void) { audio_stop(); }

void device_state_set_volume(long volume) { set_volume(volume); }

void device_state_change_volume(long amount) { change_volume(amount); }

int device_state_get_volume_percent(void) { return get_volume_percent(); }

void device_state_change_volume_percent(int delta) { change_volume_percent(delta); }

void device_state_refresh_battery(void) { sync_battery_from_sysfs(); }
