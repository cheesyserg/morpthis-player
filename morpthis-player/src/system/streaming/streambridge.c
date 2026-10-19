#include "streambridge.h"

#include "src/system/core/utils.h"
#include "src/system/net/http.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define FETCH_CHUNK (32 * 1024)
#define FETCH_TIMEOUT 10
#define READER_TIMEOUT 30
#define URL_CAP 2048
#define HEADER_PROBE_WINDOW (32 * 1024)

struct streambridge {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_t worker;
	unsigned char *data;
	size_t capacity, quota, mask, probe_window;
	uint64_t read_pos, write_pos, floor;
	unsigned generation, live_generation;
	bool pending, quit, canceled, opened, eof, failed, fetching;
	int fd; // published/retired under lock; only the worker ever closes it
	char url[URL_CAP];
	char error[192];
};

static streambridge_abort_fn reader_abort;

void streambridge_set_reader_abort(streambridge_abort_fn fn) { reader_abort = fn; }

static size_t buffered(const streambridge_t *b) {
	return b->live_generation == b->generation ? (size_t)(b->write_pos - b->read_pos) : 0;
}

static struct timespec after_ms(long ms) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec += ms / 1000;
	t.tv_nsec += (ms % 1000) * 1000000L;
	if (t.tv_nsec >= 1000000000L) {
		t.tv_sec++;
		t.tv_nsec -= 1000000000L;
	}
	return t;
}

static bool expired(struct timespec t) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec > t.tv_sec || (now.tv_sec == t.tv_sec && now.tv_nsec >= t.tv_nsec);
}

static bool current(streambridge_t *b, unsigned generation) {
	return !b->quit && !b->canceled && b->generation == generation;
}

// Retire fd under the same lock as cancellation. Otherwise shutdown(fd)
// could hit an unrelated connection which just reused a closed descriptor.
static void close_stream(streambridge_t *b, http_stream_t *stream) {
	pthread_mutex_lock(&b->lock);
	b->fd = -1;
	b->fetching = false;
	pthread_cond_broadcast(&b->changed);
	pthread_mutex_unlock(&b->lock);
	// TLS close-notify is network I/O too; no GUI/ring mutex may span it.
	http_stream_close(stream);
}

static void fail_locked(streambridge_t *b, const char *error) {
	if (b->failed) return; // retain the transfer failure, not a later codec symptom
	b->failed = true;
	b->eof = true;
	snprintf(b->error, sizeof(b->error), "%s", error);
	pthread_cond_broadcast(&b->changed);
}

typedef struct {
	streambridge_t *bridge;
	unsigned generation;
} opening_t;

static bool observe_socket(void *user, int fd) {
	opening_t *opening = user;
	streambridge_t *b = opening->bridge;
	pthread_mutex_lock(&b->lock);
	bool keep = current(b, opening->generation) && !b->failed;
	if (fd < 0) {
		b->fd = -1; // retired BEFORE close: no descriptor-reuse race
	} else if (keep) {
		b->fd = fd;
	}
	pthread_mutex_unlock(&b->lock);
	return keep;
}

static void *producer(void *arg) {
	streambridge_t *b = arg;
	// SCHED_IDLE can starve on the V1's single core while the UI redraws. Nice
	// 10 stays below playback/UI but wakes promptly to service a refill.
	thread_be_low_priority("stream bridge");
	for (;;) {
		pthread_mutex_lock(&b->lock);
		while (!b->pending && !b->quit) {
			pthread_cond_wait(&b->changed, &b->lock);
		}
		if (b->quit) {
			pthread_mutex_unlock(&b->lock);
			break;
		}
		unsigned generation = b->generation;
		char url[URL_CAP];
		memcpy(url, b->url, sizeof(url));
		memset(b->url, 0, sizeof(b->url));
		b->pending = false;
		b->live_generation = generation;
		b->read_pos = b->write_pos = b->floor = 0;
		b->opened = b->eof = b->failed = false;
		b->error[0] = '\0';
		b->fetching = true;
		pthread_mutex_unlock(&b->lock);

		http_stream_t stream;
		opening_t opening = {b, generation};
		http_stream_options_t options = {.socket_cb = observe_socket, .socket_user = &opening,
			.no_icy = true, .no_https_downgrade = true};
		bool opened = http_stream_open_ex(&stream, url, FETCH_TIMEOUT, &options);
		memset(url, 0, sizeof(url));
		pthread_mutex_lock(&b->lock);
		if (!opened) {
			if (current(b, generation)) {
				fail_locked(b, http_last_error() ? http_last_error() : "subsonic_stream_failed");
			}
			b->fetching = false;
			pthread_cond_broadcast(&b->changed);
			pthread_mutex_unlock(&b->lock);
			continue;
		}
		b->fd = stream.fd;
		if (!current(b, generation)) {
			pthread_mutex_unlock(&b->lock);
			close_stream(b, &stream);
			continue;
		}
		// Subsonic sometimes reports API errors with HTTP 200. Never hand a JSON
		// or XML failure page to a decoder, even if the track's suffix says MP3.
		char mime[sizeof(stream.content_type)];
		size_t mime_len = strcspn(stream.content_type, "; ");
		memcpy(mime, stream.content_type, mime_len);
		mime[mime_len] = '\0';
		for (size_t i = 0; i < mime_len; i++) {
			if (mime[i] >= 'A' && mime[i] <= 'Z') mime[i] += 'a' - 'A';
		}
		if (strstr(mime, "json") || strstr(mime, "xml") || strncmp(mime, "text/", 5) == 0) {
			fail_locked(b, "subsonic_stream_refused");
			pthread_mutex_unlock(&b->lock);
			close_stream(b, &stream);
			continue;
		}
		b->opened = true;
		pthread_cond_broadcast(&b->changed);
		pthread_mutex_unlock(&b->lock);

		bool burst = true;
		struct timespec stalled_until = {0};
		for (;;) {
			pthread_mutex_lock(&b->lock);
			while (current(b, generation) && !b->failed) {
				size_t used = buffered(b);
				if (used >= b->quota) {
					burst = false;
				}
				if (!burst && used <= b->quota / 2) {
					burst = true;
				}
				bool probe_guard = b->read_pos <= b->probe_window && b->write_pos >= b->capacity;
				if (burst && used < b->quota && !probe_guard) {
					break;
				}
				b->fetching = false;
				pthread_cond_wait(&b->changed, &b->lock);
			}
			if (!current(b, generation) || b->failed) {
				pthread_mutex_unlock(&b->lock);
				break;
			}
			size_t tail = (size_t)b->write_pos & b->mask;
			size_t want = b->quota - buffered(b);
			// During a recognizer's read/rewind, a fast producer could fill the
			// newly freed bytes and destroy byte zero between the two callbacks.
			// Hold the initial history until the consumer has passed its probe;
			// large ID3/FLAC headers can then skip forward beyond ring capacity.
			if (b->read_pos <= b->probe_window && (uint64_t)want > b->capacity - b->write_pos) {
				want = (size_t)(b->capacity - b->write_pos);
			}
			if (stream.content_length64 && (uint64_t)want > stream.content_length64 - b->write_pos) {
				want = (size_t)(stream.content_length64 - b->write_pos);
			}
			if (want > b->capacity - tail) {
				want = b->capacity - tail;
			}
			if (want > FETCH_CHUNK) {
				want = FETCH_CHUNK;
			}
			// Protect a retained rewind span from an in-flight socket write,
			// before exposing that write's destination outside the mutex.
			uint64_t end = b->write_pos + want;
			b->floor = end > b->capacity ? end - b->capacity : 0;
			b->fetching = true;
			pthread_mutex_unlock(&b->lock);

			errno = 0; // a stale EAGAIN must not turn a fatal TLS error into a spin
			int got = http_stream_read(&stream, b->data + tail, (int)want);
			int read_error = errno;
			pthread_mutex_lock(&b->lock);
			if (!current(b, generation) || b->failed) {
				pthread_mutex_unlock(&b->lock);
				break; // no stale bytes published into a replacement source
			}
			if (got > 0) {
				stalled_until.tv_sec = 0;
				b->write_pos += (unsigned)got;
				bool complete = stream.content_length64 > 0 && b->write_pos >= stream.content_length64;
				if (complete) {
					b->eof = true;
				}
				pthread_cond_broadcast(&b->changed);
				pthread_mutex_unlock(&b->lock);
				if (complete) {
					break;
				}
			} else {
				// A socket receive timeout is not a broken MP3/FLAC stream. Keep
				// the same connection for a bounded grace period while the RAM
				// reserve covers a Wi-Fi hiccup. HTTP retains partial chunk
				// framing too, so the same bounded retry is safe through a proxy.
				bool retry = got < 0 &&
					(read_error == EINTR || read_error == EAGAIN || read_error == EWOULDBLOCK);
				if (retry) {
					if (!stalled_until.tv_sec) {
						stalled_until = after_ms((READER_TIMEOUT - FETCH_TIMEOUT) * 1000);
					}
					if (!expired(stalled_until)) {
						pthread_mutex_unlock(&b->lock);
						continue; // recv itself sleeps; no timer/spin loop
					}
				}
				if (got < 0 || b->write_pos == 0 || (stream.content_length64 > 0 && b->write_pos < stream.content_length64)) {
					fail_locked(b, "subsonic_stream_failed");
				} else {
					b->eof = true;
					pthread_cond_broadcast(&b->changed);
				}
				pthread_mutex_unlock(&b->lock);
				break;
			}
		}
		close_stream(b, &stream);
	}
	return NULL;
}

streambridge_t *streambridge_create(size_t capacity) {
	if (!capacity || (capacity & (capacity - 1)) || capacity > 2 * STREAMBRIDGE_CAPACITY) {
		return NULL;
	}
	streambridge_t *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->data = malloc(capacity);
	if (!b->data) {
		free(b);
		return NULL;
	}
	b->capacity = b->quota = capacity;
	b->mask = capacity - 1;
	b->probe_window = capacity / 2 < HEADER_PROBE_WINDOW ? capacity / 2 : HEADER_PROBE_WINDOW;
	b->fd = -1;
	b->canceled = true;
	pthread_mutex_init(&b->lock, NULL);
	pthread_condattr_t attr;
	if (pthread_condattr_init(&attr) != 0) goto no_condition;
	int err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	if (!err) err = pthread_cond_init(&b->changed, &attr);
	pthread_condattr_destroy(&attr);
	// A realtime condvar with monotonic deadlines would busy-loop at RT
	// priority on systems lacking this support; fail rather than mis-time it.
	if (err) goto no_condition;
	pthread_attr_t worker_attr;
	err = pthread_attr_init(&worker_attr);
	if (!err) {
		// TLS/HTTP need modest stack space, not the inherited multi-MiB
		// default on a memory-constrained player. Crash signals use altstack.
		err = pthread_attr_setstacksize(&worker_attr, 256 * 1024);
		if (!err) err = pthread_create(&b->worker, &worker_attr, producer, b);
		pthread_attr_destroy(&worker_attr);
	}
	if (err) {
		pthread_cond_destroy(&b->changed);
		goto no_condition;
	}
	return b;
no_condition:
	pthread_mutex_destroy(&b->lock);
	free(b->data);
	free(b);
	return NULL;
}

void streambridge_cancel(streambridge_t *b) {
	if (!b) {
		return;
	}
	pthread_mutex_lock(&b->lock);
	b->generation++;
	b->pending = false;
	b->canceled = true;
	b->opened = false;
	memset(b->url, 0, sizeof(b->url));
	if (b->fd >= 0) {
		shutdown(b->fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&b->changed);
	pthread_mutex_unlock(&b->lock);
}

void streambridge_destroy(streambridge_t *b) {
	if (!b) {
		return;
	}
	streambridge_cancel(b);
	pthread_mutex_lock(&b->lock);
	b->quit = true;
	pthread_cond_broadcast(&b->changed);
	pthread_mutex_unlock(&b->lock);
	pthread_join(b->worker, NULL);
	pthread_cond_destroy(&b->changed);
	pthread_mutex_destroy(&b->lock);
	free(b->data);
	free(b);
}

bool streambridge_start(streambridge_t *b, const char *url, bool prefetch) {
	if (!b || !url || !url[0] || strlen(url) >= sizeof(b->url)) {
		return false;
	}
	pthread_mutex_lock(&b->lock);
	b->generation++;
	if (b->fd >= 0) {
		shutdown(b->fd, SHUT_RDWR);
	}
	snprintf(b->url, sizeof(b->url), "%s", url);
	b->quota = prefetch && b->capacity > STREAMBRIDGE_PRECACHE ? STREAMBRIDGE_PRECACHE : b->capacity;
	b->canceled = b->opened = b->eof = b->failed = false;
	b->error[0] = '\0';
	b->pending = true;
	pthread_cond_broadcast(&b->changed);
	pthread_mutex_unlock(&b->lock);
	return true;
}

void streambridge_promote(streambridge_t *b) {
	pthread_mutex_lock(&b->lock);
	b->quota = b->capacity;
	pthread_cond_broadcast(&b->changed);
	pthread_mutex_unlock(&b->lock);
}

void streambridge_state(streambridge_t *b, streambridge_state_t *out) {
	memset(out, 0, sizeof(*out));
	if (!b) {
		out->canceled = true;
		return;
	}
	pthread_mutex_lock(&b->lock);
	out->buffered = buffered(b);
	out->capacity = b->capacity;
	out->received = b->live_generation == b->generation ? b->write_pos : 0;
	out->opened = b->opened;
	out->eof = b->eof;
	out->failed = b->failed;
	out->canceled = b->canceled;
	out->fetching = b->pending || b->fetching;
	snprintf(out->error, sizeof(out->error), "%s", b->error);
	pthread_mutex_unlock(&b->lock);
}

// The abort callback may take audio_mutex, so call it OUTSIDE the ring mutex.
// Producer/GUI never wait on the audio lock while owning the ring either.
static bool aborted_locked(streambridge_t *b, streambridge_abort_fn abort) {
	if (!abort) {
		return false;
	}
	pthread_mutex_unlock(&b->lock);
	bool stop = abort();
	pthread_mutex_lock(&b->lock);
	return stop;
}

static bool wait_for(streambridge_t *b, size_t target, streambridge_abort_fn abort) {
	struct timespec deadline = after_ms(READER_TIMEOUT * 1000);
	unsigned generation = b->generation;
	while (current(b, generation) && !b->eof && buffered(b) < target) {
		if (aborted_locked(b, abort)) {
			return false;
		}
		if (!current(b, generation)) {
			break;
		}
		if (expired(deadline)) {
			fail_locked(b, "subsonic_stream_timeout");
			if (b->fd >= 0) {
				shutdown(b->fd, SHUT_RDWR);
			}
			return false;
		}
		struct timespec tick = after_ms(100);
		pthread_cond_timedwait(&b->changed, &b->lock, &tick);
	}
	return current(b, generation) && buffered(b) > 0;
}

bool streambridge_prime(streambridge_t *b) {
	pthread_mutex_lock(&b->lock);
	size_t target = b->capacity < STREAMBRIDGE_PRECACHE ? b->capacity : STREAMBRIDGE_PRECACHE;
	bool ok = wait_for(b, target, reader_abort) && !b->failed;
	pthread_mutex_unlock(&b->lock);
	return ok;
}

bool streambridge_wait_decode(streambridge_t *b, streambridge_abort_fn abort) {
	pthread_mutex_lock(&b->lock);
	bool ok = !b->canceled;
	if (buffered(b) < STREAMBRIDGE_READ_LOW && !b->eof) {
		size_t target = b->capacity < STREAMBRIDGE_PRECACHE ? b->capacity : STREAMBRIDGE_PRECACHE;
		ok = wait_for(b, target, abort);
	}
	pthread_mutex_unlock(&b->lock);
	return ok;
}

static size_t copy_locked(streambridge_t *b, void *out, size_t bytes, bool consume) {
	size_t take = buffered(b);
	if (take > bytes) {
		take = bytes;
	}
	size_t at = (size_t)b->read_pos & b->mask;
	size_t first = take < b->capacity - at ? take : b->capacity - at;
	if (out) {
		memcpy(out, b->data + at, first);
		if (take > first) {
			memcpy((unsigned char *)out + first, b->data, take - first);
		}
	}
	if (consume) {
		size_t before = buffered(b);
		b->read_pos += take;
		// A producer asleep at its high watermark wakes once per half-ring,
		// not once per tiny codec read. Forward skips/rewinds use the same rule.
		if (before > b->quota / 2 && buffered(b) <= b->quota / 2) {
			pthread_cond_broadcast(&b->changed);
		}
	}
	return take;
}

size_t streambridge_read(streambridge_t *b, void *out, size_t bytes) {
	if (!b || !out || !bytes) {
		return 0;
	}
	pthread_mutex_lock(&b->lock);
	size_t done = 0;
	unsigned generation = b->generation;
	while (done < bytes && current(b, generation)) {
		if (!buffered(b) && !wait_for(b, 1, reader_abort)) {
			break;
		}
		size_t got = copy_locked(b, (unsigned char *)out + done, bytes - done, true);
		done += got;
		if (!got) {
			break;
		}
	}
	pthread_mutex_unlock(&b->lock);
	return done;
}

size_t streambridge_peek(streambridge_t *b, void *out, size_t bytes) {
	pthread_mutex_lock(&b->lock);
	size_t got = copy_locked(b, out, bytes, false);
	pthread_mutex_unlock(&b->lock);
	return got;
}

bool streambridge_seek(streambridge_t *b, int64_t offset, bool relative) {
	pthread_mutex_lock(&b->lock);
	int64_t at = relative ? (int64_t)b->read_pos : 0;
	if ((offset < 0 && offset < -at) || (offset > 0 && at > INT64_MAX - offset)) {
		pthread_mutex_unlock(&b->lock);
		return false;
	}
	uint64_t target = (uint64_t)(at + offset);
	unsigned generation = b->generation;
	if (b->canceled || target < b->floor) {
		pthread_mutex_unlock(&b->lock);
		return false;
	}
	while (target > b->write_pos && current(b, generation)) {
		copy_locked(b, NULL, buffered(b), true); // discard metadata, without a scratch buffer
		if (!wait_for(b, 1, reader_abort)) {
			pthread_mutex_unlock(&b->lock);
			return false;
		}
	}
	bool ok = current(b, generation) && target >= b->floor && target <= b->write_pos;
	if (ok) {
		b->read_pos = target;
		pthread_cond_broadcast(&b->changed);
	}
	pthread_mutex_unlock(&b->lock);
	return ok;
}

uint64_t streambridge_tell(streambridge_t *b) {
	pthread_mutex_lock(&b->lock);
	uint64_t at = b->read_pos;
	pthread_mutex_unlock(&b->lock);
	return at;
}

bool streambridge_failed(streambridge_t *b) {
	pthread_mutex_lock(&b->lock);
	bool failed = b->failed || b->canceled;
	pthread_mutex_unlock(&b->lock);
	return failed;
}

void streambridge_fail(streambridge_t *b, const char *error) {
	pthread_mutex_lock(&b->lock);
	if (!b->canceled) {
		fail_locked(b, error);
		if (b->fd >= 0) {
			shutdown(b->fd, SHUT_RDWR);
		}
	}
	pthread_mutex_unlock(&b->lock);
}
