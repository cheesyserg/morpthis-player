#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>

// Last.fm's application credentials, read from a file on the device rather
// than written into the source.
//
// Last.fm signs every call with the API secret, so the pair has to be supplied
// by whoever assembles the firmware. The file and its paths keep their old
// names so an existing card or image still works; only the [lastfm] section is
// read now.
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
// The file is normally /usr/resource/morpthis/components/streaming-keys.bin,
// sealed by tools/seal_streamkeys.py with a key compiled into the player. A
// plain streaming-keys.ini beside it is read when there is no sealed file, and
// either form may instead sit at the root of the microSD card. The path can be
// changed with [streaming] keys_file in device_config.ini.
//
// Without one Last.fm stays off and its settings page says why.

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
// the image says.
//
// True when keys are now held that were not held before, which is the caller's
// signal to hand them to Last.fm.
bool streamkeys_load_card(const char *card_root);

// NULL when absent. The pointers stay valid for the life of the program.
const char *streamkeys_lastfm_key(void);
const char *streamkeys_lastfm_secret(void);

// Where they were read from, for the log and the information page. NULL when
// none were found.
const char *streamkeys_source(void);

#endif /* STREAMKEYS_H */
