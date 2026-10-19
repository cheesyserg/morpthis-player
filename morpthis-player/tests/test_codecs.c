// Real dr_mp3/dr_flac/libopusfile decoders consuming the production RAM bridge.
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsonicstream.h"
#include "src/system/decode/decode.h"
#include "src/system/core/config.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static bool never_abort(void) { return false; }

static void track_path(const subsonic_entry_t *track, subsonic_format_t format, char out[96]) {
	char paths[1][96], error[192];
	const char *pointers[1];
	int count, selected;
	subsonic_set_quality((subsonic_quality_t){format, 256});
	assert(subsonicstream_queue(track, 1, 0, paths, pointers, &count, &selected, error, sizeof(error)));
	assert(count == 1 && selected == 0);
	strcpy(out, paths[0]);
	assert(subsonicstream_prepare(out));
}

static void decode_track(const char *id, const char *suffix, subsonic_format_t format,
                         int rate, uint64_t frames, bool zero_pcm) {
	subsonic_entry_t track = {.kind = SUBSONIC_TRACK, .duration = 53};
	snprintf(track.id, sizeof(track.id), "%s", id);
	snprintf(track.suffix, sizeof(track.suffix), "%s", suffix);
	char path[96];
	track_path(&track, format, path);

	decoder_t *decoder = decoder_open(path, decode_detect_format(path));
	assert(decoder && decoder_sample_rate(decoder) == rate && decoder_channels(decoder) == 2);
	assert(decoder_source_bits(decoder) == 16 && decoder_is_buffered(decoder));
	// MP3 with only retained-header seeks has no Xing length: it must use the
	// server duration instead of scanning/draining the entire live stream.
	assert(decoder_total_pcm_frames(decoder) == ((format == SUBSONIC_FORMAT_MP3 && strcmp(id, "mp3-xing") != 0) || format == SUBSONIC_FORMAT_OPUS
	                                          ? (uint64_t)rate * track.duration : frames));
	assert(!decoder_seek_to_frame(decoder, 1) && decoder_seekable_limit(decoder) == 0);
	char file[512];
	double begin, end;
	assert(!decoder_source_span(decoder, file, sizeof(file), &begin, &end));

	uint64_t count = 0;
	short pcm[4096 * 2];
	for (;;) {
		assert(decoder_wait_buffered(decoder, never_abort));
		uint64_t got = decoder_read_pcm_frames_s16(decoder, 4096, pcm);
		if (!got) break;
		assert(got <= 4096 && count + got <= frames);
		if (zero_pcm) for (uint64_t i = 0; i < got * 2; i++) assert(pcm[i] == 0);
		count += got;
	}
	assert(!decoder_buffered_failed(decoder));
	if (count != frames) fprintf(stderr, "%s: expected %llu PCM frames, got %llu\n", id, (unsigned long long)frames, (unsigned long long)count);
	assert(count == frames);
	decoder_close(decoder);
	printf("PASS %s: %llu exact PCM frames via HTTP -> RAM -> real codec\n", id, (unsigned long long)count);
}

static void codec_mismatch(void) {
	subsonic_entry_t track = {.kind = SUBSONIC_TRACK, .id = "codec-mismatch", .suffix = "mp3", .duration = 3};
	char path[96], error[192];
	track_path(&track, SUBSONIC_FORMAT_MP3, path);
	assert(!decoder_open(path, decode_detect_format(path)));
	assert(subsonicstream_take_error(error, sizeof(error)) && strcmp(error, "subsonic_codec_mismatch") == 0);
	puts("PASS wrong server transcode is rejected before codec initialization");
}

static void canceled_open(void) {
	// A canceled lease must not reinterpret the service URL as a filesystem
	// path, leak a lease, or manufacture a decoder after Stop.
	subsonic_entry_t track = {.kind = SUBSONIC_TRACK, .id = "mp3", .suffix = "mp3", .duration = 53};
	char path[96];
	track_path(&track, SUBSONIC_FORMAT_MP3, path);
	subsonicstream_cancel();
	assert(!decoder_open(path, decode_detect_format(path)));
	assert(!decoder_open("subsonic://invalid/1.mp3", DECODE_FORMAT_MP3));
	puts("PASS canceled/stale stream cannot fall back to local decoder");
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	// The same arguments as the core suite: the HTTPS servers after them are
	// for the certificate cases, which have nothing to decode and live in
	// test_subsonic.c.
	assert(argc == 6);
	signal(SIGPIPE, SIG_IGN);
	streambridge_set_reader_abort(never_abort);
	char path[1024];
	assert(snprintf(path, sizeof(path), "%s/device_config.ini", argv[2]) < (int)sizeof(path));
	(void)config_init(path);
	subsonic_init();
	subsonic_credentials_t credentials = {.username = "alice+user@example.com", .password = "p &\\\"ä"};
	assert(strlen(argv[1]) < sizeof(credentials.server));
	strcpy(credentials.server, argv[1]);
	char error[192];
	assert(subsonic_configure(&credentials, false, error, sizeof(error)));

	decode_track("mp3", "mp3", SUBSONIC_FORMAT_MP3, 44100, 2000 * 1152ULL, true);
	decode_track("mp3-id3", "mp3", SUBSONIC_FORMAT_MP3, 44100, 2000 * 1152ULL, true);
	decode_track("mp3-xing", "mp3", SUBSONIC_FORMAT_MP3, 44100, 2000 * 1152ULL - 576 - 1080, true);
	decode_track("flac", "flac", SUBSONIC_FORMAT_RAW, 44100, 80 * 4096ULL, true);
	decode_track("flac-padding", "flac", SUBSONIC_FORMAT_RAW, 44100, 80 * 4096ULL, true);
	decode_track("opus", "opus", SUBSONIC_FORMAT_OPUS, 48000, 120 * 960ULL - 312 - 120, false);
	codec_mismatch();
	canceled_open();

	subsonicstream_cancel();
	subsonic_forget();
	puts("All real streaming codec tests passed.");
	return 0;
}
