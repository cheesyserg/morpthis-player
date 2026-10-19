#ifndef PODCASTDL_H
#define PODCASTDL_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/podcast.h"

// Podcast episodes kept on the card for good, as ordinary files:
//
//   <card>/Podcast/<podcast title>/<episode title>.<ext>
//
// with the podcast's artwork beside them as cover.jpg (or cover.png), each
// file's modification time set to the episode's publication date, and a
// "<file>.tags" sidecar in podcastcache.h's format, so the player treats the
// file as the episode it is. Unlike podcastcache.h nothing here is ever pruned
// or emptied.
//
// One download at a time, on a thread of its own. The file is written under a
// hidden ".<name>.part" and renamed when complete, so a cut download never
// shows up as an episode.

#define PODCASTDL_FOLDER "Podcast"

typedef enum {
	PODCASTDL_IDLE,
	PODCASTDL_RUNNING,
	PODCASTDL_DONE,
	PODCASTDL_FAILED,
	PODCASTDL_CANCELLED,
} podcastdl_state_t;

// <card>/Podcast, or "" when there is no card.
const char *podcastdl_root(void);

// Where `episode` of the podcast titled `feed_title` is saved. False when there
// is no card. Says nothing about whether the file exists.
bool podcastdl_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size);

// Whether that file is already on the card.
bool podcastdl_exists(const podcast_episode_t *episode, const char *feed_title);

// Starts downloading `episode` into the folder of `feed_title`. `feed_image` is
// the podcast's artwork, fetched once per folder; `feed_author` goes into the
// sidecar for the player's follow star. Either NULL or empty for none. False,
// starting nothing, when a download is already running, there is no card, or
// the folder cannot be made.
bool podcastdl_start(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
					 const char *feed_image);

// True when `path` is inside the Podcast folder.
bool podcastdl_owns(const char *path);

// Stops the running download and deletes what it wrote. Returns at once; the
// state becomes PODCASTDL_CANCELLED when the thread has wound up.
void podcastdl_cancel(void);

// The state, with the bytes written and the size the server announced (0 when
// it did not say). Either pointer may be NULL.
podcastdl_state_t podcastdl_state(long long *done, long long *total);

// Back to PODCASTDL_IDLE after a finished, failed or cancelled download has
// been reported.
void podcastdl_acknowledge(void);

// True while a download holds the network.
bool podcastdl_busy(void);

// Turns a title into a file or folder name the card accepts: the characters
// FAT forbids go, leading and trailing dots and spaces go, the length is capped
// on a UTF-8 boundary. `fallback` when nothing is left.
void podcastdl_safe_name(const char *title, const char *fallback, char *out, size_t size);

#endif /* PODCASTDL_H */
