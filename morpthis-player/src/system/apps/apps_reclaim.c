#include "apps_reclaim.h"

#include <malloc.h>
#include <stddef.h>
#include <stdio.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/audio/audio.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"

// Bytes the kernel says a new allocation could get, or 0 when it cannot be
// read. The same number the SDK reports to an app through
// morpthis_app_mem_available().
static size_t mem_available_bytes(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return 0;
	}

	char line[128];
	unsigned long long kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return (size_t)kb * 1024u;
}

// How long to wait for the playback thread to close the decoder and the PCM.
// The thread checks its command between periods, so this is two buffers plus
// room for a slow read from a busy card; the app is not started until it ends
// one way or the other.
#define RECLAIM_AUDIO_TIMEOUT_MS 2000

void apps_reclaim_for_heavy(const app_t *app) {
	size_t before = mem_available_bytes();

	fprintf(stderr, "apps: heavy tier, reclaiming before %s (MemAvailable %zu KB)\n",
			app ? app->name : "app", before / 1024u);

	// The one big reclaim: stop the decoder, close the PCM and release its
	// buffers. The queue is kept, so play afterwards restarts the current
	// track; nothing is lost but the position.
	audio_release_all(RECLAIM_AUDIO_TIMEOUT_MS);

	// SQLite's page caches, not the databases: the next list or cover pass
	// reads the pages back from the card.
	library_release_memory();
	audiobookdb_release_memory();
	cover_cache_release_memory();

	// The decoded artwork is the largest heap object the player keeps: the
	// player's own cover and blurred backdrop, and every list thumbnail the
	// cover loader has finished or is working on.
	player_release_artwork();
	coverloader_release_all();

	// Give the heap's now-free arenas back to the kernel, not just back to
	// malloc. Without this the freed caches still count as the process's RSS.
	malloc_trim(0);

	size_t after = mem_available_bytes();
	long long delta_kb = ((long long)after - (long long)before) / 1024;
	fprintf(stderr, "apps: reclaim done, MemAvailable %zu KB (%+lld KB)\n", after / 1024u, delta_kb);
}
