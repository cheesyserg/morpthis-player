#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>

// The streaming services' application credentials, read from a file on the
// device rather than written in here.
//
// Qobuz and Tidal offer no way to talk to their APIs without a pair of
// application keys, and the ones this device has are HiBy's: they live inside
// /usr/bin/hiby_player, Tidal's even assembled byte by byte at run time so it
// never appears as a string. They are HiBy's keys, not this project's, and open
// source that carries them publishes them.
//
// So they sit in a file on the rootfs, which whoever assembles the firmware puts
// alongside the other resource files. It is written as an INI:
//
//     [qobuz]
//     app_id = ...
//     app_secret = ...
//
//     [tidal]
//     client_id = ...
//     client_secret = ...
//
//     [podcast]
//     api_key = ...
//     api_secret = ...
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
// and shipped sealed, as /usr/resource/morpthis/components/streaming-keys.bin, by
// tools/seal_streamkeys.py. The key that opens it is not in the source: the
// tool keeps it in streaming-keys.key, outside git, and the Makefile compiles
// it into the player. That keeps the keys out of an unpacked image; it does not
// keep them from whoever takes the binary apart.
//
// A plain streaming-keys.ini next to it is still read when there is no .bin,
// for the simulator and for builds made without a key. Which of the two a file
// is, is decided by its first bytes, not its name.
//
// A third place is the microSD card: streaming-keys.bin or streaming-keys.ini
// at the card's root, read by streamkeys_load_card() once the card is mounted.
// It is there because an image published for anyone to download ships without
// keys -- there is no other way it could be published -- and the person who
// installs it should be able to turn streaming on without building a firmware.
// Copy one file to the card and restart.
//
// Without any of them, or without a section, the matching service disables
// itself and its menu entry says so instead of failing halfway through a login.
//
// The path can be changed with [streaming] keys_file in device_config.ini, to
// keep the keys on the card instead of the read-only rootfs -- the easy path
// the day a service changes its keys.

#include "src/system/core/respath.h"

#define STREAMKEYS_PATH MORPTHIS_RESOURCE_DIR "/components/streaming-keys.bin"
#define STREAMKEYS_PLAIN_PATH MORPTHIS_RESOURCE_DIR "/components/streaming-keys.ini"

// The two names looked for at the card's root, in this order.
#define STREAMKEYS_CARD_BIN "streaming-keys.bin"
#define STREAMKEYS_CARD_INI "streaming-keys.ini"

// Reads the file. Call once at startup, after config_init(). A missing file is
// not an error.
void streamkeys_init(void);

// Looks for the same two files at the root of the microSD card: .bin first,
// then .ini. Call once, after the card is mounted, which is long after
// streamkeys_init() has run -- nothing mounts the card before the player does,
// so at startup there is nothing to read there yet.
//
// Only a firmware that found no keys of its own is affected: one that ships a
// sealed streaming-keys.bin keeps it, and a card cannot quietly replace what
// the image says. That is what makes this safe to leave in a release.
//
// True when keys are now held that were not held before, which is the caller's
// signal to hand them to the services that read them at their own startup.
bool streamkeys_load_card(const char *card_root);

// NULL when absent. The pointers stay valid for the life of the program.
const char *streamkeys_qobuz_app_id(void);
const char *streamkeys_qobuz_app_secret(void);
const char *streamkeys_tidal_client_id(void);
const char *streamkeys_tidal_client_secret(void);
// Podcast Index. This pair identifies this player rather than a HiBy
// application: it is requested free on podcastindex.org, and each request is
// signed with the SHA-1 of key + secret + time. Same rule as the other two:
// out of the source, in a file whoever assembles the firmware fills in.
const char *streamkeys_podcast_key(void);
const char *streamkeys_podcast_secret(void);
// Last.fm. An API account of this player, requested on last.fm/api: it signs
// every call with the MD5 of the sorted parameters plus the secret.
const char *streamkeys_lastfm_key(void);
const char *streamkeys_lastfm_secret(void);

// Where they were read from, for the log and the information page. NULL when
// none were found.
const char *streamkeys_source(void);

#endif /* STREAMKEYS_H */
