#include "subsonic.h"

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define API_BODY_LIMIT (2 * 1024 * 1024)
#define API_TIMEOUT 12

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t credentials_io_lock = PTHREAD_MUTEX_INITIALIZER;
static subsonic_credentials_t account;
static subsonic_quality_t quality = {SUBSONIC_FORMAT_MP3, 256};
static bool remembered;
static unsigned api_inflight;
static char credentials_path[600];

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text);
	}
}

static bool valid_text(const char *text, size_t capacity) {
	return text && strnlen(text, capacity) < capacity;
}

static bool normalize(subsonic_credentials_t *c, char *error, size_t size) {
	if (!valid_text(c->server, sizeof(c->server)) || !valid_text(c->username, sizeof(c->username)) ||
		!valid_text(c->password, sizeof(c->password)) || !c->username[0] || !c->password[0]) {
		reason(error, size, "subsonic_invalid_credentials");
		return false;
	}
	bool secure = strncasecmp(c->server, "https://", 8) == 0;
	if (!secure && strncasecmp(c->server, "http://", 7) != 0) {
		reason(error, size, "subsonic_invalid_url");
		return false;
	}
	memcpy(c->server, secure ? "https://" : "http://", secure ? 8 : 7);
	const char *authority = c->server + (secure ? 8 : 7);
	if (!*authority || *authority == '/' || strpbrk(authority, "@?#\\")) {
		reason(error, size, "subsonic_invalid_url");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; *p; p++) {
		if (isspace(*p) || *p < 32 || *p == 127) {
			reason(error, size, "subsonic_invalid_url");
			return false;
		}
	}
	const char *end = strchr(authority, '/');
	if (!end) end = authority + strlen(authority);
	const char *colon = memchr(authority, ':', (size_t)(end - authority));
	const char *host_end = colon ? colon : end;
	if (host_end == authority || host_end - authority >= 256) {
		reason(error, size, "subsonic_invalid_url");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; p < (const unsigned char *)host_end; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') {
			reason(error, size, "subsonic_invalid_url");
			return false;
		}
	}
	if (colon) {
		unsigned port = 0;
		for (const char *p = colon + 1; p < end; p++) {
			if (*p < '0' || *p > '9' || (port = port * 10 + (unsigned)(*p - '0')) > 65535) {
				reason(error, size, "subsonic_invalid_url");
				return false;
			}
		}
		if (!port) {
			reason(error, size, "subsonic_invalid_url");
			return false;
		}
	}
	size_t len = strlen(c->server);
	while (len && c->server[len - 1] == '/') {
		c->server[--len] = '\0';
	}
	// Accept a pasted /rest URL as well as the documented server base.
	if (len >= 5 && strcmp(c->server + len - 5, "/rest") == 0) {
		c->server[len - 5] = '\0';
	}
	return true;
}

static bool write_string(FILE *f, const char *s) {
	if (fputc('"', f) == EOF) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (fputc('\\', f) == EOF || fputc(*p, f) == EOF) {
				return false;
			}
		} else if (*p < 32) {
			if (fprintf(f, "\\u%04x", *p) < 0) {
				return false;
			}
		} else if (fputc(*p, f) == EOF) {
			return false;
		}
	}
	return fputc('"', f) != EOF;
}

static bool save_credentials(const subsonic_credentials_t *c) {
	if (!credentials_path[0]) {
		return false;
	}
	char tmp[sizeof(credentials_path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", credentials_path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	if (fchmod(fd, 0600) != 0) {
		close(fd);
		unlink(tmp);
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	bool ok = fputs("{\"server\":", f) >= 0 && write_string(f, c->server) &&
			  fputs(",\"username\":", f) >= 0 && write_string(f, c->username) &&
			  fputs(",\"password\":", f) >= 0 && write_string(f, c->password) && fputs("}\n", f) >= 0;
	if (fflush(f) != 0 || fsync(fd) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok) {
		ok = rename(tmp, credentials_path) == 0;
	}
	if (!ok) {
		unlink(tmp);
	}
	return ok;
}

void subsonic_init(void) {
	credentials_path[0] = '\0';
	const char *path = config_path();
	if (path && path[0] && strlen(path) < sizeof(credentials_path) - 40) {
		snprintf(credentials_path, sizeof(credentials_path), "%s", path);
		char *slash = strrchr(credentials_path, '/');
		if (slash) {
			slash[1] = '\0';
		} else {
			credentials_path[0] = '\0';
		}
		strcat(credentials_path, "subsonic-credentials.json");
	}
	subsonic_credentials_t loaded = {0};
	char text[8192];
	int fd = credentials_path[0] ? open(credentials_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
	bool have = false;
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)sizeof(text)) {
			FILE *f = fdopen(fd, "r");
			if (f) {
				size_t n = fread(text, 1, sizeof(text) - 1, f);
				text[n] = '\0';
				json_doc_t doc;
				if (!ferror(f) && json_parse(text, &doc)) {
					int root = json_root(&doc);
					have = json_str_exact(&doc, json_get(&doc, root, "server"), loaded.server, sizeof(loaded.server)) &&
						   json_str_exact(&doc, json_get(&doc, root, "username"), loaded.username, sizeof(loaded.username)) &&
						   json_str_exact(&doc, json_get(&doc, root, "password"), loaded.password, sizeof(loaded.password)) &&
						   normalize(&loaded, NULL, 0);
					json_free(&doc);
				}
				fclose(f);
				fd = -1;
			}
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	int format = (int)config_get_int("subsonic", "format", SUBSONIC_FORMAT_MP3);
	int bitrate = (int)config_get_int("subsonic", "max_bitrate", 256);
	pthread_mutex_lock(&lock);
	account = have ? loaded : (subsonic_credentials_t){0};
	remembered = have;
	quality.format = format >= SUBSONIC_FORMAT_MP3 && format <= SUBSONIC_FORMAT_RAW ? format : SUBSONIC_FORMAT_MP3;
	quality.max_bitrate = bitrate == 320 ? 320 : 256;
	pthread_mutex_unlock(&lock);
	memset(&loaded, 0, sizeof(loaded));
	memset(text, 0, sizeof(text));
}

bool subsonic_configure(const subsonic_credentials_t *credentials, bool remember, char *error, size_t size) {
	reason(error, size, "");
	if (!credentials) {
		reason(error, size, "subsonic_invalid_credentials");
		return false;
	}
	subsonic_credentials_t c = *credentials;
	if (!normalize(&c, error, size)) {
		return false;
	}
	pthread_mutex_lock(&credentials_io_lock);
	bool saved = remember ? save_credentials(&c) :
		!credentials_path[0] || unlink(credentials_path) == 0 || errno == ENOENT;
	if (saved) {
		pthread_mutex_lock(&lock);
		account = c;
		remembered = remember;
		pthread_mutex_unlock(&lock);
	}
	pthread_mutex_unlock(&credentials_io_lock);
	memset(&c, 0, sizeof(c));
	if (!saved) reason(error, size, "subsonic_cannot_save_credentials");
	return saved;
}

void subsonic_credentials(subsonic_credentials_t *out, bool *saved) {
	pthread_mutex_lock(&lock);
	if (out) {
		*out = account;
	}
	if (saved) {
		*saved = remembered;
	}
	pthread_mutex_unlock(&lock);
}

bool subsonic_configured(void) {
	pthread_mutex_lock(&lock);
	bool ok = account.server[0] && account.username[0] && account.password[0];
	pthread_mutex_unlock(&lock);
	return ok;
}

bool subsonic_forget(void) {
	pthread_mutex_lock(&credentials_io_lock);
	// Serialize removal with saving; never report a successful Forget if the
	// plaintext file remains and could reload this account at the next boot.
	bool removed = !credentials_path[0] || unlink(credentials_path) == 0 || errno == ENOENT;
	pthread_mutex_lock(&lock);
	memset(&account, 0, sizeof(account));
	remembered = false;
	pthread_mutex_unlock(&lock);
	pthread_mutex_unlock(&credentials_io_lock);
	return removed;
}

subsonic_quality_t subsonic_quality(void) {
	pthread_mutex_lock(&lock);
	subsonic_quality_t copy = quality;
	pthread_mutex_unlock(&lock);
	return copy;
}

void subsonic_set_quality(subsonic_quality_t q) {
	if (q.format < SUBSONIC_FORMAT_MP3 || q.format > SUBSONIC_FORMAT_RAW) {
		q.format = SUBSONIC_FORMAT_MP3;
	}
	q.max_bitrate = q.max_bitrate == 320 ? 320 : 256;
	pthread_mutex_lock(&lock);
	quality = q;
	pthread_mutex_unlock(&lock);
	config_set_int("subsonic", "format", q.format);
	config_set_int("subsonic", "max_bitrate", q.max_bitrate);
	config_save();
}

static bool request_url(const char *endpoint, const char *key, const char *value,
						char *out, size_t size, char *error, size_t error_size) {
	subsonic_credentials_t c;
	subsonic_credentials(&c, NULL);
	if (!c.server[0] || !c.username[0] || !c.password[0]) {
		reason(error, error_size, "subsonic_not_configured");
		return false;
	}
	unsigned char random[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t done = 0;
	while (fd >= 0 && done < sizeof(random)) {
		ssize_t n = read(fd, random + done, sizeof(random) - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (done != sizeof(random)) {
		reason(error, error_size, "subsonic_random_failed");
		memset(&c, 0, sizeof(c));
		return false; // never weaken authentication with a clock-derived salt
	}
	static const char hex[] = "0123456789abcdef";
	char salt[33], token[MD5_HEX_LEN];
	for (size_t i = 0; i < sizeof(random); i++) {
		salt[i * 2] = hex[random[i] >> 4];
		salt[i * 2 + 1] = hex[random[i] & 15];
	}
	salt[32] = '\0';
	char signed_text[SUBSONIC_PASSWORD_MAX + sizeof(salt)];
	snprintf(signed_text, sizeof(signed_text), "%s%s", c.password, salt);
	md5_hex(signed_text, strlen(signed_text), token);
	memset(signed_text, 0, sizeof(signed_text));
	char user[SUBSONIC_USER_MAX * 3 + 4], encoded[SUBSONIC_ID_MAX * 3 + 4];
	http_url_encode(c.username, user, sizeof(user));
	if (key && (!value || strlen(value) >= SUBSONIC_ID_MAX)) {
		reason(error, error_size, "subsonic_invalid_id");
		memset(&c, 0, sizeof(c));
		return false;
	}
	http_url_encode(value, encoded, sizeof(encoded));
	int n = snprintf(out, size, "%s/rest/%s.view?u=%s&t=%s&s=%s&v=1.16.1&c=morpthis&f=json%s%s%s",
					 c.server, endpoint, user, token, salt, key ? "&" : "", key ? key : "", key ? "=" : "");
	bool ok = n > 0 && (size_t)n < size;
	if (ok && key) {
		size_t used = (size_t)n;
		ok = strlen(encoded) < size - used;
		if (ok) {
			memcpy(out + used, encoded, strlen(encoded) + 1);
		}
	}
	memset(&c, 0, sizeof(c));
	if (!ok) {
		reason(error, error_size, "subsonic_invalid_url");
	}
	return ok;
}

bool subsonic_stream_url(const char *id, subsonic_quality_t q, char *out, size_t size, char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!out || !size || !id || !id[0] || q.format < SUBSONIC_FORMAT_MP3 || q.format > SUBSONIC_FORMAT_RAW ||
		!request_url("stream", "id", id, out, size, error, error_size)) {
		return false;
	}
	size_t used = strlen(out);
	const char *format = q.format == SUBSONIC_FORMAT_RAW ? "raw" : q.format == SUBSONIC_FORMAT_OPUS ? "opus" : "mp3";
	int n = q.format == SUBSONIC_FORMAT_RAW ? snprintf(out + used, size - used, "&format=%s", format) :
			 snprintf(out + used, size - used, "&format=%s&maxBitRate=%d", format, q.max_bitrate == 320 ? 320 : 256);
	if (n < 0 || (size_t)n >= size - used) {
		reason(error, error_size, "subsonic_invalid_url");
		return false;
	}
	return true;
}

static bool api(const char *endpoint, const char *key, const char *value, char **body,
				json_doc_t *doc, int *response, char *error, size_t size) {
	reason(error, size, "");
	char url[SUBSONIC_URL_MAX];
	if (!request_url(endpoint, key, value, url, sizeof(url), error, size)) {
		return false;
	}
	int status = 0;
	size_t len = 0;
	http_req_t request = {.want_error_body = true, .no_https_downgrade = true};
	pthread_mutex_lock(&lock);
	api_inflight++;
	pthread_mutex_unlock(&lock);
	bool fetched = http_request(url, &request, body, &len, API_BODY_LIMIT + 1, API_TIMEOUT, &status);
	memset(url, 0, sizeof(url));
	pthread_mutex_lock(&lock);
	api_inflight--;
	pthread_mutex_unlock(&lock);
	if (status == 401 || status == 403) {
		free(*body);
		*body = NULL;
		reason(error, size, "subsonic_auth_failed");
		return false;
	}
	if (!fetched) {
		reason(error, size, http_last_error() ? http_last_error() : "subsonic_connection_failed");
		return false;
	}
	if (len > API_BODY_LIMIT || !json_parse_limited(*body, doc, 65536)) {
		reason(error, size, len >= API_BODY_LIMIT ? "subsonic_response_too_large" : "subsonic_invalid_response");
		free(*body);
		*body = NULL;
		return false;
	}
	*response = json_get(doc, json_root(doc), "subsonic-response");
	char result[24];
	bool ok = status == 200 && json_obj_str(doc, *response, "status", result, sizeof(result)) && strcmp(result, "ok") == 0;
	if (!ok) {
		int detail = json_get(doc, *response, "error");
		long code = json_obj_long(doc, detail, "code", 0);
		// Authentication errors get an actionable, translatable answer.
		if (code == 40 || code == 41 || code == 42 || code == 43 || code == 44 || status == 401 || status == 403) {
			reason(error, size, "subsonic_auth_failed");
		} else {
			char message[192] = "";
			json_obj_str(doc, detail, "message", message, sizeof(message));
			reason(error, size, message[0] ? message : "subsonic_invalid_response");
		}
		json_free(doc);
		free(*body);
		*body = NULL;
	}
	return ok;
}

bool subsonic_ping(char *error, size_t size) {
	char *body = NULL;
	json_doc_t doc;
	int response;
	if (!api("ping", NULL, NULL, &body, &doc, &response, error, size)) {
		return false;
	}
	json_free(&doc);
	free(body);
	return true;
}

static bool identifier(const json_doc_t *doc, int obj, char *out, size_t size) {
	int tok = json_get(doc, obj, "id");
	if (tok < 0) {
		return false;
	}
	if (doc->toks[tok].type == JSON_NUMBER) {
		const json_tok_t *t = &doc->toks[tok];
		size_t len = (size_t)(t->end - t->start);
		if (!len || len >= size) return false;
		for (size_t i = 0; i < len; i++) {
			if (doc->text[t->start + i] < '0' || doc->text[t->start + i] > '9') return false;
		}
		// Preserve integer IDs verbatim even above INT64_MAX; do not turn a
		// fractional/exponent form or an overflow into a different identity.
		memcpy(out, doc->text + t->start, len);
		out[len] = '\0';
		return true;
	}
	// Never silently cut an opaque identifier down to a different identifier.
	return json_str_exact(doc, tok, out, size) && out[0];
}

static bool entry(const json_doc_t *doc, int obj, subsonic_entry_kind_t kind, subsonic_entry_t *out) {
	memset(out, 0, sizeof(*out));
	out->kind = kind;
	if (!identifier(doc, obj, out->id, sizeof(out->id))) {
		return false;
	}
	if (!json_obj_str(doc, obj, "title", out->title, sizeof(out->title))) {
		json_obj_str(doc, obj, "name", out->title, sizeof(out->title));
	}
	if (!out->title[0]) {
		snprintf(out->title, sizeof(out->title), "%s", out->id);
	}
	json_obj_str(doc, obj, "artist", out->artist, sizeof(out->artist));
	json_obj_str(doc, obj, "album", out->album, sizeof(out->album));
	json_obj_str(doc, obj, "suffix", out->suffix, sizeof(out->suffix));
	long duration = json_obj_long(doc, obj, "duration", 0);
	out->duration = duration > 0 && duration <= INT_MAX ? (int)duration : 0;
	out->track_number = (int)json_obj_long(doc, obj, "track", 0);
	return true;
}

typedef struct {
	subsonic_entry_t *entries;
	int offset, limit, seen, count;
	bool more;
} page_t;

static void append(const json_doc_t *doc, int obj, subsonic_entry_kind_t kind, page_t *page) {
	subsonic_entry_t item;
	if (!entry(doc, obj, kind, &item)) {
		return;
	}
	if (page->seen++ < page->offset) {
		return;
	}
	if (page->count < page->limit) {
		page->entries[page->count++] = item;
	} else {
		page->more = true;
	}
}

static void array(const json_doc_t *doc, int arr, subsonic_entry_kind_t kind, bool mixed, page_t *page) {
	// Linear token walk rather than repeated json_at(0..n), which re-scans the
	// preceding entries each time. Subtrees are skipped by their end offsets.
	if (arr < 0 || doc->toks[arr].type != JSON_ARRAY) {
		return;
	}
	for (int i = arr + 1; i < doc->count && doc->toks[i].start < doc->toks[arr].end && !page->more;) {
		int obj = i;
		subsonic_entry_kind_t actual = mixed ? (json_obj_bool(doc, obj, "isDir", false) ? SUBSONIC_DIRECTORY : SUBSONIC_TRACK) : kind;
		append(doc, obj, actual, page);
		int end = doc->toks[obj].end;
		do { i++; } while (i < doc->count && doc->toks[i].start < end);
	}
}

int subsonic_browse(subsonic_browse_t browse, const char *id, int offset, subsonic_entry_t *entries,
					int limit, bool *more, char *error, size_t size) {
	if (more) {
		*more = false;
	}
	if (!entries || limit <= 0 || limit > SUBSONIC_PAGE_SIZE || offset < 0) {
		reason(error, size, "subsonic_invalid_response");
		return -1;
	}
	const char *endpoint, *key = NULL;
	switch (browse) {
	case SUBSONIC_BROWSE_FOLDERS: endpoint = "getMusicFolders"; break;
	case SUBSONIC_BROWSE_INDEXES: endpoint = "getIndexes"; key = "musicFolderId"; break;
	case SUBSONIC_BROWSE_DIRECTORY: endpoint = "getMusicDirectory"; key = "id"; break;
	case SUBSONIC_BROWSE_ARTISTS: endpoint = "getArtists"; break;
	case SUBSONIC_BROWSE_ARTIST: endpoint = "getArtist"; key = "id"; break;
	case SUBSONIC_BROWSE_ALBUM: endpoint = "getAlbum"; key = "id"; break;
	default: return -1;
	}
	char *body = NULL;
	json_doc_t doc;
	int response;
	if (!api(endpoint, key, id, &body, &doc, &response, error, size)) {
		return -1;
	}
	page_t page = {entries, offset, limit, 0, 0, false};
	if (browse == SUBSONIC_BROWSE_ARTISTS || browse == SUBSONIC_BROWSE_INDEXES) {
		int root = json_get(&doc, response, browse == SUBSONIC_BROWSE_ARTISTS ? "artists" : "indexes");
		int indexes = json_get(&doc, root, "index");
		for (int i = 0; i < json_len(&doc, indexes) && !page.more; i++) {
			int index = json_at(&doc, indexes, i);
			array(&doc, json_get(&doc, index, "artist"), browse == SUBSONIC_BROWSE_ARTISTS ? SUBSONIC_ARTIST : SUBSONIC_DIRECTORY, false, &page);
		}
		// Loose songs directly inside a music folder, where the server has them.
		if (browse == SUBSONIC_BROWSE_INDEXES) {
			array(&doc, json_get(&doc, root, "child"), SUBSONIC_TRACK, true, &page);
		}
	} else if (browse == SUBSONIC_BROWSE_FOLDERS) {
		array(&doc, json_path(&doc, response, "musicFolders.musicFolder"), SUBSONIC_MUSIC_FOLDER, false, &page);
	} else if (browse == SUBSONIC_BROWSE_DIRECTORY) {
		array(&doc, json_path(&doc, response, "directory.child"), SUBSONIC_TRACK, true, &page);
	} else if (browse == SUBSONIC_BROWSE_ARTIST) {
		array(&doc, json_path(&doc, response, "artist.album"), SUBSONIC_ALBUM, false, &page);
	} else {
		array(&doc, json_path(&doc, response, "album.song"), SUBSONIC_TRACK, false, &page);
	}
	if (more) {
		*more = page.more;
	}
	json_free(&doc);
	free(body);
	return page.count;
}

bool subsonic_api_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = api_inflight > 0;
	pthread_mutex_unlock(&lock);
	return busy;
}
