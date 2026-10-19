#ifndef SUBSONICSTREAM_H
#define SUBSONICSTREAM_H

#include "subsonic.h"
#include "streambridge.h"
#include "src/system/library/metadata.h"

#include <stdbool.h>
#include <stddef.h>

// Two fixed 1 MiB slots, allocated lazily. The next slot holds only its first
// 512 KiB until promoted. Queues contain credential-free, ephemeral local
// source names, NOT HTTP URLs; the audio side cannot accidentally do I/O.
bool subsonicstream_owns(const char *path);
bool subsonicstream_queue(const subsonic_entry_t *tracks, int count, int start,
						 char paths[][96], const char **pointers, int *queued, int *selected,
						 char *error, size_t error_size);

// GUI thread, registered as a device_state prepare callback. Starts/promotes
// a producer without waiting. Paths from other services cancel our fetches.
bool subsonicstream_prepare(const char *path);
void subsonicstream_prefetch(const char *path); // GUI thread, at 90% progress
void subsonicstream_cancel(void); // any thread; no join or socket read
void subsonicstream_reset(void); // account change: also invalidate old queue IDs
bool subsonicstream_network_wanted(void);

// Decoder lease: the slot cannot be reused/reset until released. Acquisition
// never opens an HTTP connection; all bytes come from its producer's RAM ring.
streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate);
void subsonicstream_release(streambridge_t *bridge);
bool subsonicstream_metadata(const char *path, song_metadata_t *out);
// Reports the current slot's error once to the GUI, including a failed open.
bool subsonicstream_take_error(char *out, size_t size);

#endif
