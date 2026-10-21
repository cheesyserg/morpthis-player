#ifndef APPS_RECLAIM_H
#define APPS_RECLAIM_H

#include "src/system/apps/apps.h"

// The heavy app tier.
//
// Called before a heavy app is loaded. It stops playback and waits for the
// decoder and the ALSA stream to be gone, gives SQLite's page caches back, and
// returns the heap's free pages to the kernel. The queue and the open database
// handles stay as they are; the caches refill from the card on the next query.
//
// A light app never calls this: the player keeps playing and keeps its caches.
void apps_reclaim_for_heavy(const app_t *app);

#endif /* APPS_RECLAIM_H */
