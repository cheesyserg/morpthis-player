#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

// Settings that survive a power cycle.
//
// On the device this lives at /usr/data/device_config.ini -- the UBIFS
// partition, which is the only writable place that is always there (the rootfs
// is read-only and the card may not be inserted). The format is a plain INI so
// it can be read and edited over ADB without any tooling:
//
//     [ui]
//     theme = dark
//
//     [clock]
//     configured = 1
//     last_seen = 1786000000
//
// Everything is held in memory; config_save() rewrites the file atomically, so
// a power cut in the middle of a write cannot leave a truncated config behind.

// Loads the file. A missing or unreadable file is not an error: the defaults
// passed to the getters are used and the file appears on the first save.
bool config_init(const char *path);

const char *config_get(const char *section, const char *key, const char *fallback);
long config_get_int(const char *section, const char *key, long fallback);
bool config_get_bool(const char *section, const char *key, bool fallback);

// The setters only touch memory. Call config_save() to put them on disk.
void config_set(const char *section, const char *key, const char *value);
void config_set_int(const char *section, const char *key, long value);
void config_set_bool(const char *section, const char *key, bool value);

// Writes the file if anything changed since the last save. Returns false when
// the file could not be written (read-only filesystem, no space).
bool config_save(void);

// Where the settings are being kept, or NULL if config_init() was never called.
const char *config_path(void);

// ---------------------------------------------------------------------------
// The store
//
// Everything above operates on the main store. These are the same operations
// behind a handle, used by the wrappers above.
// ---------------------------------------------------------------------------

typedef struct config_store config_store_t;

bool config_store_init(config_store_t *store, const char *path);

const char *config_store_get(config_store_t *store, const char *section, const char *key, const char *fallback);
long config_store_get_int(config_store_t *store, const char *section, const char *key, long fallback);
void config_store_set(config_store_t *store, const char *section, const char *key, const char *value);
void config_store_set_int(config_store_t *store, const char *section, const char *key, long value);
bool config_store_save(config_store_t *store);

#endif /* CONFIG_H */
