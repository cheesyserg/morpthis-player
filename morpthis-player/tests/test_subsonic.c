#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsonicstream.h"
#include "src/system/streaming/streambridge.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/net/http.h"
#include "src/system/net/tls.h"
#include "src/system/playback/playlist.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char base[512];
static _Atomic bool abort_read;
static bool should_abort(void) { return atomic_load(&abort_read); }

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void url(char *out, size_t size, const char *path) {
	assert(snprintf(out, size, "%s%s", base, path) < (int)size);
}

static streambridge_state_t wait_buffer(streambridge_t *bridge, size_t wanted) {
	double end = now() + 3;
	streambridge_state_t state;
	do {
		streambridge_state(bridge, &state);
		if (state.buffered >= wanted || state.eof || state.failed) {
			return state;
		}
		usleep(1000);
	} while (now() < end);
	assert(!"buffer did not fill within three seconds");
	return state;
}

static void check_pattern(const unsigned char *bytes, size_t count, size_t offset, unsigned seed) {
	for (size_t i = 0; i < count; i++) {
		assert(bytes[i] == (unsigned char)(offset + i + seed));
	}
}

static void test_client(const char *directory) {
	char config_file[1024], error[192], stream_url[SUBSONIC_URL_MAX];
	snprintf(config_file, sizeof(config_file), "%s/device_config.ini", directory);
	(void)config_init(config_file); // missing file means defaults, as at first boot
	subsonic_init();
	assert(!subsonic_configured());
	assert(!subsonic_ping(error, sizeof(error)));
	assert(strcmp(error, "subsonic_not_configured") == 0);

	subsonic_credentials_t c = {0};
	assert(snprintf(c.server, sizeof(c.server), "%s/rest/", base) < (int)sizeof(c.server));
	strcpy(c.username, "alice+user@example.com");
	strcpy(c.password, "p &\\\"ä");
	assert(subsonic_configure(&c, true, error, sizeof(error)));
	char saved_path[1024];
	snprintf(saved_path, sizeof(saved_path), "%s/subsonic-credentials.json", directory);
	struct stat st;
	assert(stat(saved_path, &st) == 0 && (st.st_mode & 0777) == 0600);
	assert(subsonic_configured());
	assert(subsonic_ping(error, sizeof(error)));
	assert(!subsonic_api_busy());

	subsonic_credentials_t saved;
	bool remembered;
	subsonic_credentials(&saved, &remembered);
	assert(remembered && strcmp(saved.server, base) == 0);
	assert(strcmp(saved.password, c.password) == 0);
	subsonic_init(); // real JSON persistence, including escaped UTF-8 credentials
	subsonic_credentials(&saved, &remembered);
	assert(remembered && strcmp(saved.password, c.password) == 0);

	subsonic_quality_t q = subsonic_quality();
	assert(q.format == SUBSONIC_FORMAT_MP3 && q.max_bitrate == 256);
	assert(subsonic_stream_url("song/1?+&= ü", q, stream_url, sizeof(stream_url), error, sizeof(error)));
	assert(strstr(stream_url, "/rest/stream.view?u=alice%2Buser%40example.com&t="));
	assert(strstr(stream_url, "&id=song%2F1%3F%2B%26%3D%20%C3%BC"));
	assert(strstr(stream_url, "&format=mp3&maxBitRate=256"));
	assert(!strstr(stream_url, "&p=") && !strstr(stream_url, c.password));
	char previous[SUBSONIC_URL_MAX];
	strcpy(previous, stream_url);
	assert(subsonic_stream_url("song/1?+&= ü", q, stream_url, sizeof(stream_url), error, sizeof(error)));
	assert(strcmp(previous, stream_url) != 0); // a fresh random salt per request
	q.format = SUBSONIC_FORMAT_OPUS;
	q.max_bitrate = 320;
	assert(subsonic_stream_url("song", q, stream_url, sizeof(stream_url), error, sizeof(error)));
	assert(strstr(stream_url, "&format=opus&maxBitRate=320"));
	q.format = SUBSONIC_FORMAT_RAW;
	assert(subsonic_stream_url("song", q, stream_url, sizeof(stream_url), error, sizeof(error)));
	assert(strstr(stream_url, "&format=raw") && !strstr(stream_url, "maxBitRate"));
	char small[40];
	assert(!subsonic_stream_url("song", q, small, sizeof(small), error, sizeof(error)));
	char huge_id[SUBSONIC_ID_MAX + 4];
	memset(huge_id, 'a', sizeof(huge_id) - 1); huge_id[sizeof(huge_id) - 1] = 0;
	assert(!subsonic_stream_url(huge_id, q, stream_url, sizeof(stream_url), error, sizeof(error)));

	subsonic_entry_t entries[SUBSONIC_PAGE_SIZE];
	bool more;
	int n = subsonic_browse(SUBSONIC_BROWSE_FOLDERS, NULL, 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 2 && !more && entries[0].kind == SUBSONIC_MUSIC_FOLDER && strcmp(entries[0].id, "7") == 0);
	assert(strcmp(entries[1].id, "123456789012345678901234567890") == 0);
	n = subsonic_browse(SUBSONIC_BROWSE_INDEXES, "7", 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 2 && entries[0].kind == SUBSONIC_DIRECTORY && entries[1].kind == SUBSONIC_TRACK);
	assert(strcmp(entries[0].id, "folder/A&B") == 0);
	n = subsonic_browse(SUBSONIC_BROWSE_DIRECTORY, "folder/A&B", 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 3 && entries[0].kind == SUBSONIC_DIRECTORY && entries[1].kind == SUBSONIC_TRACK);
	assert(strcmp(entries[1].id, "song/1?+") == 0 && entries[1].duration == 180 && entries[1].track_number == 1);
	assert(strcmp(entries[1].title, "Música ♪") == 0);
	n = subsonic_browse(SUBSONIC_BROWSE_ARTISTS, NULL, 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == SUBSONIC_PAGE_SIZE && more && entries[0].kind == SUBSONIC_ARTIST);
	n = subsonic_browse(SUBSONIC_BROWSE_ARTISTS, NULL, SUBSONIC_PAGE_SIZE, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 5 && !more && strcmp(entries[0].id, "artist/200") == 0);
	n = subsonic_browse(SUBSONIC_BROWSE_ARTIST, "artist/0", 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 1 && entries[0].kind == SUBSONIC_ALBUM && strcmp(entries[0].id, "album/0") == 0);
	n = subsonic_browse(SUBSONIC_BROWSE_ALBUM, "album/0", 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error));
	assert(n == 2 && entries[0].kind == SUBSONIC_TRACK);

	const char *bad_albums[] = {"auth-error", "auth-text", "bad-json", "too-large", "too-many-tokens"};
	const char *failures[] = {"subsonic_auth_failed", "subsonic_auth_failed", "subsonic_invalid_response", "subsonic_response_too_large", "subsonic_invalid_response"};
	for (size_t i = 0; i < sizeof(bad_albums) / sizeof(bad_albums[0]); i++) {
		assert(subsonic_browse(SUBSONIC_BROWSE_ALBUM, bad_albums[i], 0, entries, SUBSONIC_PAGE_SIZE, &more, error, sizeof(error)) == -1);
		if (strcmp(error, failures[i])) fprintf(stderr, "%s: expected %s, got %s\n", bad_albums[i], failures[i], error);
		assert(strcmp(error, failures[i]) == 0 && !subsonic_api_busy());
	}

	strcpy(c.password, "wrong");
	assert(subsonic_configure(&c, false, error, sizeof(error)));
	assert(!subsonic_ping(error, sizeof(error)) && strcmp(error, "subsonic_auth_failed") == 0);
	strcpy(c.password, "p &\\\"ä");
	assert(subsonic_configure(&c, false, error, sizeof(error)));
	assert(stat(saved_path, &st) != 0); // unchecking remember removes old secrets
	assert(subsonic_ping(error, sizeof(error)));
	strcpy(saved.server, "http://bad.example/path?token=secret");
	assert(!subsonic_configure(&saved, false, error, sizeof(error)));
	strcpy(saved.server, "file:///etc/passwd");
	assert(!subsonic_configure(&saved, false, error, sizeof(error)));
	const char *bad_servers[] = {"https://user:pass@example.com", "http://example.com:1oops", "http://example.com:+80",
		"http://example.com:-80", "http://example.com:", "http://example.com:0", "http://example.com:65536", "http://:80",
		"http://example.com/space here", "http://example.com/path#fragment", "http://[::1]", "http:///rest"};
	for (size_t i = 0; i < sizeof(bad_servers) / sizeof(bad_servers[0]); i++) {
		strcpy(saved.server, bad_servers[i]);
		assert(!subsonic_configure(&saved, false, error, sizeof(error)) && strcmp(error, "subsonic_invalid_url") == 0);
	}

	// Never load a truncated identity/password, a symlink, or embedded NUL.
	FILE *f = fopen(saved_path, "w");
	assert(f);
	fprintf(f, "{\"server\":\"%s\",\"username\":\"%s\",\"password\":\"p\"}", base, huge_id);
	assert(fclose(f) == 0);
	subsonic_init();
	assert(!subsonic_configured());
	f = fopen(saved_path, "w");
	assert(f);
	fprintf(f, "{\"server\":\"%s\",\"username\":\"alice\",\"password\":\"p\\u0000ignored\"}", base);
	assert(fclose(f) == 0);
	subsonic_init();
	assert(!subsonic_configured());
	assert(unlink(saved_path) == 0 && symlink(config_file, saved_path) == 0);
	subsonic_init();
	assert(!subsonic_configured());
	assert(subsonic_configure(&c, false, error, sizeof(error)));
	assert(subsonic_forget());
	assert(mkdir(saved_path, 0700) == 0); // an unlink failure, even when the test runs as root
	assert(!subsonic_forget() && !subsonic_configured());
	assert(rmdir(saved_path) == 0);
	assert(subsonic_configure(&c, false, error, sizeof(error)));
	puts("PASS REST/authentication/URL encoding/JSON browsing/pagination/credential permissions");
}

// The three HTTPS servers the harness starts with the certificates in
// tls_fixtures.py: one self-signed and correctly named, one naming another
// server, one expired. What a home Subsonic server presents, and the two
// things that must stay refusals even when the chain requirement is lowered.
static void test_tls(const char *good, const char *wrong_name, const char *expired) {
	// tls_available() is also the loading: when this host has no OpenSSL at all
	// there is nothing to test and the player refuses every https:// URL, which
	// is itself the documented behaviour. Say so rather than pass quietly.
	(void)tls_available();
	if (!tls_version() || !tls_version()[0]) {
		puts("SKIP TLS: this host has no loadable OpenSSL; every https:// is refused here");
		return;
	}

	char url[512], *body = NULL;
	size_t length = 0;
	const char *reason;

	// The shipped default: a certificate from no authority the bundle knows is
	// refused, and the caller is told why.
	tls_set_verify_mode(TLS_VERIFY_STRICT);
	assert(tls_verify_mode() == TLS_VERIFY_STRICT);
	snprintf(url, sizeof(url), "%s/pattern?length=64&seed=3", good);
	assert(!http_get(url, &body, &length, 4096, 10));
	reason = http_last_error();
	assert(reason && strncmp(reason, "tls_", 4) == 0); // the certificate, not the network
	assert(body == NULL);

	// Relaxed, which is what the switch writes: the same server now connects.
	tls_set_verify_mode(TLS_VERIFY_RELAXED);
	assert(tls_verify_mode() == TLS_VERIFY_RELAXED);
	assert(http_get(url, &body, &length, 4096, 10));
	assert(length == 64);
	check_pattern((const unsigned char *)body, length, 0, 3); // the body came from this server
	free(body);
	body = NULL;

	// And only the chain is forgiven. A certificate issued to another name, and
	// one whose dates do not cover now, fail exactly as they do in strict mode.
	snprintf(url, sizeof(url), "%s/pattern?length=64&seed=3", wrong_name);
	assert(!http_get(url, &body, &length, 4096, 10));
	assert(strcmp(http_last_error(), "tls_cert_name_mismatch") == 0);
	snprintf(url, sizeof(url), "%s/pattern?length=64&seed=3", expired);
	assert(!http_get(url, &body, &length, 4096, 10));
	assert(strcmp(http_last_error(), "tls_cert_expired") == 0);

	// A connection already open keeps working when the setting changes under it:
	// the shared context is rebuilt, and this stream holds its own reference to
	// the one it was opened with.
	snprintf(url, sizeof(url), "%s/pattern?length=64&seed=3", good);
	tls_set_verify_mode(TLS_VERIFY_RELAXED);
	http_stream_t stream;
	assert(http_stream_open(&stream, url, 10));
	tls_set_verify_mode(TLS_VERIFY_STRICT);
	assert(tls_verify_mode() == TLS_VERIFY_STRICT);
	char buffer[64];
	int n = http_stream_read(&stream, buffer, sizeof(buffer));
	assert(n > 0);
	check_pattern((const unsigned char *)buffer, (size_t)n, 0, 3);
	http_stream_close(&stream);

	// Where the next boot reads it from: the switch is not a memory-only setting.
	assert(strcmp(config_get("network", "tls_verify", ""), "strict") == 0);
	puts("PASS TLS: strict refuses a self-signed certificate, relaxed accepts it, "
		 "name/date failures stay failures and a live stream survives the switch");
}

static void test_json_exact(void) {
	json_doc_t doc;
	char out[5];
	assert(json_parse("\"abcd\"", &doc));
	assert(json_str_exact(&doc, 0, out, sizeof(out)) && strcmp(out, "abcd") == 0);
	assert(!json_str_exact(&doc, 0, out, sizeof(out) - 1) && out[0] == 0);
	assert(json_str(&doc, 0, out, sizeof(out) - 1) && strcmp(out, "abc") == 0); // display API unchanged
	json_free(&doc);
	assert(json_parse("\"a\\u0000b\"", &doc));
	assert(!json_str_exact(&doc, 0, out, sizeof(out)) && out[0] == 0);
	json_free(&doc);
	assert(json_parse("\"\\ud83c\\udfb5\"", &doc));
	assert(json_str_exact(&doc, 0, out, sizeof(out)) && strcmp(out, "🎵") == 0);
	assert(!json_str_exact(&doc, 0, out, sizeof(out) - 1) && out[0] == 0);
	json_free(&doc);
	assert(json_parse("\"\\ud800\"", &doc));
	assert(!json_str_exact(&doc, 0, out, sizeof(out)) && out[0] == 0);
	json_free(&doc);
	assert(json_parse("\"a\\q\"", &doc));
	assert(!json_str_exact(&doc, 0, out, sizeof(out)) && out[0] == 0);
	json_free(&doc);
	assert(json_parse("\"a\\uZZZZ\"", &doc));
	assert(!json_str_exact(&doc, 0, out, sizeof(out)) && out[0] == 0);
	json_free(&doc);
	assert(!json_parse_limited("[]", &doc, 0) && doc.toks == NULL);
	puts("PASS exact JSON identity extraction: UTF-8/length/NUL/surrogates/escapes, lenient display API unchanged");
}

static void test_ring(void) {
	assert(!streambridge_create(0) && !streambridge_create(1000));
	streambridge_t *bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
	assert(bridge);
	char address[1024];
	url(address, sizeof(address), "/pattern?length=6291555&seed=13");
	assert(streambridge_start(bridge, address, false) && streambridge_prime(bridge));
	streambridge_state_t state = wait_buffer(bridge, STREAMBRIDGE_CAPACITY);
	assert(state.buffered == STREAMBRIDGE_CAPACITY && !state.failed);
	uint64_t received = state.received;
	unsigned char bytes[8191];
	assert(streambridge_read(bridge, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 13);
	usleep(50000);
	streambridge_state(bridge, &state);
	assert(state.received == received); // remains asleep above its 50% low watermark
	assert(streambridge_seek(bridge, 0, false)); // retained header bytes
	assert(streambridge_read(bridge, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 13);
	assert(streambridge_tell(bridge) == sizeof(bytes));

	size_t done = sizeof(bytes);
	for (;;) {
		size_t got = streambridge_read(bridge, bytes, sizeof(bytes));
		if (!got) { break; }
		check_pattern(bytes, got, done, 13);
		done += got;
		streambridge_state(bridge, &state);
		assert(state.buffered <= state.capacity);
	}
	assert(done == 6291555 && !streambridge_failed(bridge));
	assert(!streambridge_seek(bridge, 0, false)); // overwritten history is not seekable
	streambridge_state(bridge, &state);
	assert(state.eof && !state.failed);

	url(address, sizeof(address), "/pattern?length=3145728&seed=71");
	assert(streambridge_start(bridge, address, true) && streambridge_prime(bridge));
	state = wait_buffer(bridge, STREAMBRIDGE_PRECACHE);
	assert(state.buffered == STREAMBRIDGE_PRECACHE && state.received == STREAMBRIDGE_PRECACHE);
	usleep(50000);
	streambridge_state(bridge, &state);
	assert(state.received == STREAMBRIDGE_PRECACHE); // exact next-track cap
	streambridge_promote(bridge);
	state = wait_buffer(bridge, STREAMBRIDGE_CAPACITY);
	assert(state.buffered == STREAMBRIDGE_CAPACITY);
	assert(streambridge_read(bridge, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 71);
	streambridge_cancel(bridge);
	assert(streambridge_read(bridge, bytes, sizeof(bytes)) == 0);
	streambridge_destroy(bridge);
	puts("PASS fixed RAM bound/ring wrap/50% refill/retained header rewind/512 KiB pre-cache promotion");
}

static void test_header_race(void) {
	streambridge_t *bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
	assert(bridge);
	char address[1024];
	url(address, sizeof(address), "/header-race");
	assert(streambridge_start(bridge, address, false) && streambridge_prime(bridge));
	unsigned char probe[64];
	assert(streambridge_read(bridge, probe, sizeof(probe)) == sizeof(probe));
	check_pattern(probe, sizeof(probe), 0, 8);
	// Producer completes its initial burst between recognizer callbacks.
	wait_buffer(bridge, STREAMBRIDGE_CAPACITY - sizeof(probe));
	usleep(50000);
	streambridge_state_t state;
	streambridge_state(bridge, &state);
	assert(state.received == STREAMBRIDGE_CAPACITY && streambridge_tell(bridge) == sizeof(probe));
	assert(streambridge_seek(bridge, 0, SEEK_SET));
	assert(streambridge_read(bridge, probe, sizeof(probe)) == sizeof(probe));
	check_pattern(probe, sizeof(probe), 0, 8);
	streambridge_destroy(bridge);
	puts("PASS initial header read/rewind is protected from a racing full-quota producer");
}

static void test_transfer_edges(void) {
	streambridge_t *bridge = streambridge_create(64 * 1024);
	assert(bridge);
	char address[1024];
	unsigned char bytes[733];
	url(address, sizeof(address), "/chunked?length=177777&seed=29");
	assert(streambridge_start(bridge, address, false) && streambridge_prime(bridge));
	size_t done = 0;
	for (;;) {
		size_t got = streambridge_read(bridge, bytes, sizeof(bytes));
		if (!got) { break; }
		check_pattern(bytes, got, done, 29);
		done += got;
	}
	assert(done == 177777 && !streambridge_failed(bridge));
	url(address, sizeof(address), "/chunked-broken");
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge));
	assert(streambridge_failed(bridge));
	const char *malformed[] = {"sign", "space", "overflow", "long-line", "no-zero", "bad-crlf", "no-trailer-end", "nul", "long-trailers"};
	for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
		char path[128];
		snprintf(path, sizeof(path), "/chunked-malformed?variant=%s", malformed[i]);
		url(address, sizeof(address), path);
		assert(streambridge_start(bridge, address, false));
		assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	}
	url(address, sizeof(address), "/truncated");
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	streambridge_fail(bridge, "subsonic_codec_mismatch");
	streambridge_state_t failed;
	streambridge_state(bridge, &failed);
	assert(strcmp(failed.error, "subsonic_stream_failed") == 0); // don't mask the first network error
	url(address, sizeof(address), "/huge-length");
	http_stream_t stream;
	assert(http_stream_open(&stream, address, 2) && stream.content_length64 == UINT64_C(4294967403));
	http_stream_close(&stream);
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	url(address, sizeof(address), "/empty");
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	url(address, sizeof(address), "/stream-error");
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	url(address, sizeof(address), "/not-found");
	assert(streambridge_start(bridge, address, false));
	assert(!streambridge_prime(bridge) && streambridge_failed(bridge));
	streambridge_destroy(bridge);
	puts("PASS chunked transfer/trailers/truncation/empty streams/HTTP and protocol errors");
}

static void *blocked_reader(void *arg) {
	unsigned char bytes[128 * 1024];
	size_t got = streambridge_read(arg, bytes, sizeof(bytes));
	assert(got <= 8192);
	return NULL;
}

static void test_radio_chunked(void) {
	char address[1024];
	url(address, sizeof(address), "/radio-chunked");
	http_stream_t stream;
	assert(http_stream_open(&stream, address, 2) && stream.icy_metaint == 8);
	unsigned char bytes[37];
	size_t count = 0;
	for (;;) {
		int got = http_stream_read(&stream, bytes, sizeof(bytes));
		if (!got) break;
		assert(got > 0);
		check_pattern(bytes, (size_t)got, count, 55);
		count += (size_t)got;
	}
	assert(count == 128 && stream.chunk_finished && strcmp(stream.stream_title, "Test radio") == 0 && stream.title_changed);
	http_stream_close(&stream);
	puts("PASS existing radio ICY stripping/title updates across chunk boundaries");
}

static void test_chunk_retry(void) {
	char address[1024];
	url(address, sizeof(address), "/chunked-retry");
	http_stream_t stream;
	http_stream_options_t options = {.no_icy = true};
	assert(http_stream_open_ex(&stream, address, 1, &options));
	unsigned char bytes[256];
	size_t count = 0;
	int retries = 0;
	for (;;) {
		int got = http_stream_read(&stream, bytes, sizeof(bytes));
		if (!got) break;
		if (got < 0) {
			assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
			assert(++retries < 8);
			continue;
		}
		check_pattern(bytes, (size_t)got, count, 77);
		count += (size_t)got;
	}
	assert(count == 256 && retries >= 3 && stream.chunk_finished && !stream.chunk_failed);
	http_stream_close(&stream);
	puts("PASS same-connection chunked retry after partial size/CRLF/trailer timeouts");
}

static void test_cancellation(void) {
	streambridge_t *bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
	char address[1024];
	url(address, sizeof(address), "/headers-stall");
	assert(streambridge_start(bridge, address, false));
	usleep(50000); // server accepts but never sends headers
	double start = now();
	url(address, sizeof(address), "/pattern?length=700000&seed=101");
	assert(streambridge_start(bridge, address, false) && streambridge_prime(bridge));
	assert(now() - start < 1.0); // setup socket canceled, not its 10-second timeout
	unsigned char bytes[100];
	assert(streambridge_read(bridge, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 101); // no bytes from an obsolete generation
	streambridge_cancel(bridge);

	url(address, sizeof(address), "/body-stall");
	assert(streambridge_start(bridge, address, false));
	wait_buffer(bridge, 8192);
	pthread_t reader;
	assert(pthread_create(&reader, NULL, blocked_reader, bridge) == 0);
	usleep(50000);
	start = now();
	streambridge_cancel(bridge);
	pthread_join(reader, NULL);
	assert(now() - start < 0.25);

	assert(streambridge_start(bridge, address, false));
	wait_buffer(bridge, 8192);
	atomic_store(&abort_read, true);
	start = now();
	assert(!streambridge_prime(bridge) && now() - start < 0.25);
	atomic_store(&abort_read, false);
	streambridge_destroy(bridge);
	puts("PASS cancellation during HTTP setup/body starvation/reader wait and stale-generation isolation");
}

static void test_playlist(void) {
	const char *paths[] = {"subsonic://1/0.mp3", "subsonic://1/1.mp3", "subsonic://1/2.mp3"};
	char peek[128], next[128];
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(paths, 3, 0);
	assert(playlist_peek_auto(peek, sizeof(peek)) && strcmp(peek, paths[1]) == 0);
	assert(playlist_current_index() == 0);
	assert(playlist_advance_auto(next, sizeof(next)) && strcmp(peek, next) == 0);
	assert(playlist_set_current(2) && !playlist_peek_auto(peek, sizeof(peek)));
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	assert(playlist_peek_auto(peek, sizeof(peek)) && strcmp(peek, paths[0]) == 0);
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(playlist_peek_auto(peek, sizeof(peek)) && strcmp(peek, paths[2]) == 0);
	assert(playlist_set_current(0));
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	assert(playlist_peek_auto(peek, sizeof(peek)));
	assert(playlist_advance_auto(next, sizeof(next)) && strcmp(peek, next) == 0);
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	assert(playlist_set_current(2) && !playlist_peek_auto(peek, sizeof(peek)));
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	playlist_load_paths_ordered(paths, 3, 2);
	assert(!playlist_peek_auto(peek, sizeof(peek)));
	playlist_clear();
	puts("PASS nonmutating next-track look-ahead in normal/repeat/shuffle/ordered queues");
}

static void test_leases(void) {
	subsonic_entry_t tracks[2] = {{.kind = SUBSONIC_TRACK, .id = "blob-a", .title = "First", .suffix = "flac", .duration = 180},
		{.kind = SUBSONIC_TRACK, .id = "blob-b", .title = "Second", .suffix = "flac", .duration = 180}};
	char paths[2][96], error[192];
	const char *pointers[2];
	int count, selected;
	subsonic_set_quality((subsonic_quality_t){SUBSONIC_FORMAT_MP3, 256});
	assert(subsonicstream_queue(tracks, 2, 0, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(count == 2 && selected == 0 && subsonicstream_owns(paths[0]));
	assert(!strstr(paths[0], "alice") && !strstr(paths[0], "&t="));
	assert(subsonicstream_prepare(paths[0]));
	int duration, bitrate;
	streambridge_t *first = subsonicstream_acquire(paths[0], &duration, &bitrate);
	assert(first && duration == 180 && bitrate == 256 && streambridge_prime(first));
	assert(!subsonicstream_acquire(paths[0], NULL, NULL)); // one decoder lease per slot
	wait_buffer(first, STREAMBRIDGE_CAPACITY);
	subsonicstream_prefetch(paths[1]);
	usleep(100000);
	assert(subsonicstream_prepare(paths[1]));
	double start = now();
	streambridge_t *second = subsonicstream_acquire(paths[1], NULL, NULL);
	assert(second && second != first && streambridge_prime(second) && now() - start < 0.2);
	subsonicstream_release(first); // cannot cancel the already promoted next slot
	unsigned char bytes[999];
	assert(streambridge_read(second, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 91);
	song_metadata_t metadata;
	assert(subsonicstream_metadata(paths[1], &metadata) && strcmp(metadata.title, "Second") == 0);
	assert(!subsonicstream_metadata("/local/song.flac", &metadata));
	wait_buffer(second, STREAMBRIDGE_CAPACITY - sizeof(bytes)); // retain the initial probe history
	subsonicstream_prefetch(paths[1]); // repeat-one must fetch a separate stream at byte zero
	usleep(100000);
	assert(subsonicstream_prepare(paths[1]));
	streambridge_t *repeat = subsonicstream_acquire(paths[1], NULL, NULL);
	assert(repeat && repeat != second && streambridge_prime(repeat));
	subsonicstream_release(second);
	assert(streambridge_read(repeat, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 91);
	subsonicstream_release(repeat);

	// The other completion order: codec teardown can win the race with the
	// GUI's automatic advance. It must retain the already cached spare.
	assert(subsonicstream_queue(tracks, 2, 0, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(subsonicstream_prepare(paths[0]));
	first = subsonicstream_acquire(paths[0], NULL, NULL);
	assert(first && streambridge_prime(first));
	wait_buffer(first, STREAMBRIDGE_CAPACITY);
	subsonicstream_prefetch(paths[1]);
	usleep(100000);
	subsonicstream_release(first);
	assert(subsonicstream_network_wanted()); // don't park Wi-Fi in the handoff gap
	assert(subsonicstream_prepare(paths[1]));
	start = now();
	second = subsonicstream_acquire(paths[1], NULL, NULL);
	assert(second && streambridge_prime(second) && now() - start < 0.2);
	assert(streambridge_read(second, bytes, sizeof(bytes)) == sizeof(bytes));
	check_pattern(bytes, sizeof(bytes), 0, 91);
	subsonicstream_release(second);

	// A failed next-track pre-cache must not make the two-Hz GUI start a new
	// HTTP request repeatedly or keep Wi-Fi wanted after playback stops.
	strcpy(tracks[1].id, "stream-fail");
	assert(subsonicstream_queue(tracks, 2, 0, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(subsonicstream_prepare(paths[0]));
	first = subsonicstream_acquire(paths[0], NULL, NULL);
	assert(first && streambridge_prime(first));
	wait_buffer(first, STREAMBRIDGE_CAPACITY);
	for (int i = 0; i < 6; i++) {
		subsonicstream_prefetch(paths[1]);
		usleep(50000);
	}
	subsonicstream_release(first);
	assert(!subsonicstream_network_wanted());
	subsonicstream_cancel();
	assert(!subsonicstream_take_error(error, sizeof(error)));

	subsonic_set_quality((subsonic_quality_t){SUBSONIC_FORMAT_RAW, 256});
	strcpy(tracks[1].suffix, "dsf");
	assert(!subsonicstream_queue(tracks, 2, 1, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(strcmp(error, "subsonic_raw_unsupported") == 0);
	assert(subsonicstream_queue(tracks, 2, 0, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(count == 1 && strstr(paths[0], ".flac"));
	assert(subsonicstream_prepare(paths[0]));
	first = subsonicstream_acquire(paths[0], NULL, NULL);
	assert(first); // reset while an outgoing decoder still owns its source
	subsonicstream_reset();
	assert(streambridge_failed(first) && subsonicstream_prepare(paths[0]));
	assert(!subsonicstream_acquire(paths[0], NULL, NULL));
	assert(subsonicstream_take_error(error, sizeof(error)) && strcmp(error, "subsonic_invalid_id") == 0);
	assert(subsonicstream_metadata(paths[0], &metadata) && metadata.title[0] == 0);
	subsonicstream_release(first);
	assert(!subsonicstream_acquire(paths[0], NULL, NULL));
	subsonicstream_cancel();
	puts("PASS two-slot ownership/instant pre-cache handoff/repeat-one/metadata/raw codec constraints");
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	// http base, config directory, then the three HTTPS servers the harness
	// starts: self-signed, wrong name, expired.
	assert(argc == 6);
	signal(SIGPIPE, SIG_IGN); // TLS/socket code, same as the application
	assert(strlen(argv[1]) < sizeof(base));
	strcpy(base, argv[1]);
	streambridge_set_reader_abort(should_abort);
	json_doc_t doc;
	assert(!json_parse_limited("[1,2,3]", &doc, 3) && doc.toks == NULL);
	assert(json_parse_limited("[1,2,3]", &doc, 4));
	json_free(&doc);
	test_json_exact();
	test_client(argv[2]);
	test_tls(argv[3], argv[4], argv[5]);
	test_ring();
	test_header_race();
	test_transfer_edges();
	test_chunk_retry();
	test_radio_chunked();
	test_cancellation();
	test_playlist();
	test_leases();
	subsonic_forget();
	assert(!subsonic_configured());
	puts("All Subsonic/bridge integration tests passed.");
	return 0;
}
