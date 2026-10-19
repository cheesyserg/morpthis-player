#ifndef STREAMBRIDGE_H
#define STREAMBRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A local, bounded byte source. One persistent producer owns every socket/TLS
// operation. The decoder copies contiguous RAM spans, at most two at a wrap.
// No temporary file, loopback HTTP server, per-frame allocation or busy loop.
#define STREAMBRIDGE_CAPACITY (1024 * 1024)
#define STREAMBRIDGE_PRECACHE (512 * 1024)
#define STREAMBRIDGE_READ_LOW (64 * 1024)

typedef struct streambridge streambridge_t;
typedef bool (*streambridge_abort_fn)(void);

typedef struct {
	size_t buffered;
	size_t capacity;
	uint64_t received;
	bool opened;
	bool eof;
	bool failed;
	bool canceled;
	bool fetching;
	char error[192];
} streambridge_state_t;

// Power-of-two capacity, at most 2 MiB: wrap is a mask, not a 64-bit divide
// on the MIPS audio thread. Production uses exactly STREAMBRIDGE_CAPACITY.
streambridge_t *streambridge_create(size_t capacity);
void streambridge_destroy(streambridge_t *bridge); // join; teardown/tests only

// Nonblocking, even while the worker is in recv(). A generation counter drops
// all stale writes and wakes readers. Caller must not replace a leased source.
// `prefetch` stops at 512 KiB (or capacity if smaller) until promotion.
bool streambridge_start(streambridge_t *bridge, const char *url, bool prefetch);
void streambridge_promote(streambridge_t *bridge);
void streambridge_cancel(streambridge_t *bridge);
void streambridge_state(streambridge_t *bridge, streambridge_state_t *out);

// Register once, at audio_init. This aborts on stop/replacement/shutdown, NOT
// pause: returning EOF from inside a codec on pause would poison its state.
void streambridge_set_reader_abort(streambridge_abort_fn fn);

// Initial reserve: 512 KiB or an actually finished short file. Waits on a
// condition variable, never a socket. Abort checks only during starvation.
bool streambridge_prime(streambridge_t *bridge);

// Refill hysteresis before the next decode block: under 64 KiB wait for
// 512 KiB (or EOF), rather than trickling a few bytes per underrun. An optional
// abort includes pause/seek, since no codec is in progress at this boundary.
bool streambridge_wait_decode(streambridge_t *bridge, streambridge_abort_fn abort);

size_t streambridge_read(streambridge_t *bridge, void *out, size_t bytes);
// For native FLAC headers: forward skips, and rewinds only to bytes still in
// the ring. End-relative seeks and arbitrary playback seeks are unsupported.
bool streambridge_seek(streambridge_t *bridge, int64_t offset, bool relative);
uint64_t streambridge_tell(streambridge_t *bridge);
size_t streambridge_peek(streambridge_t *bridge, void *out, size_t bytes);
bool streambridge_failed(streambridge_t *bridge);
void streambridge_fail(streambridge_t *bridge, const char *error);

#endif
