#include "debug.h"

#include <stdlib.h>

#include "src/system/core/config.h"
#include "src/system/core/logging.h"

// Written by the interface thread when a switch moves, read by the interface
// loop, the pointer callback and the library scan. Relaxed is enough: the value
// is one independent bool, and a switch that lands a pass late harms nothing.
static bool debug_on;

bool debug_enabled(void) { return __atomic_load_n(&debug_on, __ATOMIC_RELAXED); }

void debug_refresh(void) {
	bool on;

#ifdef HOST_BUILD
	// The simulator is a development machine: its output is the point.
	on = true;
#else
	const char *forced = getenv("MORPTHIS_DEBUG");
	const char *log_path = getenv("MORPTHIS_LOG");
	if ((forced && forced[0]) || (log_path && log_path[0])) {
		on = true;
	} else {
		on = config_get_bool("system", "adb", false) || config_get_bool("system", "log_to_sd", false) ||
			 config_get_bool("bluetooth", "log", false) || config_get_bool("library", "log_database", false);
	}
#endif

	__atomic_store_n(&debug_on, on, __ATOMIC_RELAXED);

	// The Bluetooth log receives the player's own lines through a tap on
	// stdout, so it needs the stamped stream even with the microSD log off.
	// Going the other way, the stamps and their timestamp work are removed.
	logging_refresh_capture();
}
