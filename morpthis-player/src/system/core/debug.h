#ifndef MORPTHIS_DEBUG_H
#define MORPTHIS_DEBUG_H

#include <stdbool.h>
#include <stdio.h>

// Whether anything that exists to look inside the player is switched on:
// ADB, the microSD log, the Bluetooth log or the database log.
//
// Everything diagnostic asks this once and does nothing when it is false,
// rather than formatting a line that ends up in /dev/null. It is cached:
// config_get_bool() walks the settings table, which is not something a scroll
// handler or a render pass should do. The switches call debug_refresh() when
// they change, and main() calls it once after the config has been read.
bool debug_enabled(void);

// Re-reads the switches. Also brings the log's capture streams in or out of
// line, because the Bluetooth log taps stdout even when the microSD log is off.
void debug_refresh(void);

// Diagnostic output. The condition is evaluated before the arguments are
// formatted, so a disabled build pays for a cached bool and nothing else.
#define DEBUG_LOG(...)                                                                                                 \
	do {                                                                                                               \
		if (debug_enabled()) {                                                                                         \
			printf(__VA_ARGS__);                                                                                       \
		}                                                                                                              \
	} while (0)

#define DEBUG_ERR(...)                                                                                                 \
	do {                                                                                                               \
		if (debug_enabled()) {                                                                                         \
			fprintf(stderr, __VA_ARGS__);                                                                              \
		}                                                                                                              \
	} while (0)

#endif /* MORPTHIS_DEBUG_H */
