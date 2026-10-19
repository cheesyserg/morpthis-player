#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// Subsonic / Gonic / Navidrome. All REST calls are blocking and belong on the
// page's worker, never on the GUI or playback thread. No application API key.
#define SUBSONIC_SERVER_MAX 512
#define SUBSONIC_USER_MAX 128
#define SUBSONIC_PASSWORD_MAX 256
#define SUBSONIC_ID_MAX 128
#define SUBSONIC_URL_MAX 2048
#define SUBSONIC_PAGE_SIZE 200

typedef struct {
	char server[SUBSONIC_SERVER_MAX]; // base URL, with any reverse-proxy prefix
	char username[SUBSONIC_USER_MAX];
	char password[SUBSONIC_PASSWORD_MAX];
} subsonic_credentials_t;

typedef enum {
	SUBSONIC_FORMAT_MP3,
	SUBSONIC_FORMAT_OPUS,
	SUBSONIC_FORMAT_RAW, // original file, NOT decoded PCM
} subsonic_format_t;

typedef struct {
	subsonic_format_t format;
	int max_bitrate; // 256 or 320 kbps; omitted entirely for raw
} subsonic_quality_t;

typedef enum {
	SUBSONIC_MUSIC_FOLDER,
	SUBSONIC_DIRECTORY,
	SUBSONIC_ARTIST,
	SUBSONIC_ALBUM,
	SUBSONIC_TRACK,
} subsonic_entry_kind_t;

typedef struct {
	subsonic_entry_kind_t kind;
	char id[SUBSONIC_ID_MAX]; // opaque strings, not necessarily numeric
	char title[200];
	char artist[128];
	char album[200];
	char suffix[16];
	int duration;
	int track_number;
} subsonic_entry_t;

typedef enum {
	SUBSONIC_BROWSE_FOLDERS,
	SUBSONIC_BROWSE_INDEXES,
	SUBSONIC_BROWSE_DIRECTORY,
	SUBSONIC_BROWSE_ARTISTS,
	SUBSONIC_BROWSE_ARTIST,
	SUBSONIC_BROWSE_ALBUM,
} subsonic_browse_t;

// Called once after config_init. The optional credential file lives alongside
// device_config.ini, is JSON, and is atomically saved with mode 0600. Passwords
// are remembered ONLY when requested. HTTPS is still required for privacy:
// the salted MD5 token mandated by Subsonic is a reusable credential too.
void subsonic_init(void);
bool subsonic_configure(const subsonic_credentials_t *credentials, bool remember,
						char *error, size_t error_size);
void subsonic_credentials(subsonic_credentials_t *out, bool *remembered);
bool subsonic_configured(void);
// Clears memory even if removal fails; false means the saved file remains.
bool subsonic_forget(void);
subsonic_quality_t subsonic_quality(void);
void subsonic_set_quality(subsonic_quality_t quality); // GUI thread; saves config

bool subsonic_ping(char *error, size_t error_size);
bool subsonic_api_busy(void);

// Pages of at most SUBSONIC_PAGE_SIZE entries, bounded JSON response memory.
// getArtists/getIndexes have no server-side pagination in the protocol: offset
// is applied while walking the JSON tokens. `more` is explicit, never a silent
// truncation. Empty successful directories return 0, errors return -1.
int subsonic_browse(subsonic_browse_t browse, const char *id, int offset,
					subsonic_entry_t *entries, int limit, bool *more,
					char *error, size_t error_size);

// URL construction only, no network. Fresh /dev/urandom salt, token auth,
// v=1.16.1, c=morpthis, f=json; no plaintext password in a URL or a log.
bool subsonic_stream_url(const char *id, subsonic_quality_t quality,
						 char *out, size_t size, char *error, size_t error_size);

#endif
