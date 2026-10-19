#include "lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/library/metadata.h"

#define LYRICS_FILE_MAX (256 * 1024)
#define LYRICS_LINES_MAX 4000

// ---------------------------------------------------------------------------
// The text, as UTF-8
// ---------------------------------------------------------------------------

static bool valid_utf8(const unsigned char *p, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = p[i];
		size_t follow;
		if (c < 0x80) {
			follow = 0;
		} else if ((c & 0xE0) == 0xC0) {
			follow = 1;
		} else if ((c & 0xF0) == 0xE0) {
			follow = 2;
		} else if ((c & 0xF8) == 0xF0) {
			follow = 3;
		} else {
			return false;
		}
		if (i + follow >= len && follow) {
			return false;
		}
		for (size_t k = 1; k <= follow; k++) {
			if ((p[i + k] & 0xC0) != 0x80) {
				return false;
			}
		}
		i += follow + 1;
	}
	return true;
}

static size_t put_utf8(char *out, uint32_t cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

// A malloc'd, NUL-terminated UTF-8 copy of what a file holds.
static char *to_utf8(const unsigned char *data, size_t len) {
	if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		data += 3;
		len -= 3;
	}

	if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
		bool big = data[0] == 0xFE;
		char *out = malloc(len * 2 + 1);
		if (!out) {
			return NULL;
		}
		size_t o = 0;
		for (size_t i = 2; i + 1 < len; i += 2) {
			uint32_t u = big ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
			if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
				uint32_t lo = big ? ((uint32_t)data[i + 2] << 8 | data[i + 3]) : ((uint32_t)data[i + 3] << 8 | data[i + 2]);
				if (lo >= 0xDC00 && lo < 0xE000) {
					u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
					i += 2;
				}
			}
			if (u == 0) {
				break;
			}
			o += put_utf8(out + o, u);
		}
		out[o] = '\0';
		return out;
	}

	if (valid_utf8(data, len)) {
		char *out = malloc(len + 1);
		if (out) {
			memcpy(out, data, len);
			out[len] = '\0';
		}
		return out;
	}

	// Neither: Latin-1, which is what a desktop that is not asked writes.
	char *out = malloc(len * 2 + 1);
	if (!out) {
		return NULL;
	}
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		o += put_utf8(out + o, data[i]);
	}
	out[o] = '\0';
	return out;
}

static char *read_text_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	unsigned char *buf = malloc(LYRICS_FILE_MAX);
	size_t len = buf ? fread(buf, 1, LYRICS_FILE_MAX, f) : 0;
	fclose(f);
	char *text = len > 0 ? to_utf8(buf, len) : NULL;
	free(buf);
	return text;
}

// ---------------------------------------------------------------------------
// Where the text comes from
// ---------------------------------------------------------------------------

// "<dir>/<name>.<ext>", with a name that cannot hold a slash.
static char *read_beside(const char *path, const char *name, size_t name_len, const char *ext) {
	if (name_len == 0 || memchr(name, '/', name_len)) {
		return NULL;
	}
	const char *slash = strrchr(path, '/');
	size_t dir_len = slash ? (size_t)(slash - path) : 0;
	char candidate[1024];
	int n = snprintf(candidate, sizeof(candidate), "%.*s%s%.*s.%s", (int)dir_len, path, slash ? "/" : "", (int)name_len,
					 name, ext);
	if (n <= 0 || (size_t)n >= sizeof(candidate)) {
		return NULL;
	}
	return read_text_file(candidate);
}

static char *read_by_names(const char *path, const char *title, const char *ext) {
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	const char *dot = strrchr(base, '.');
	size_t base_len = dot && dot != base ? (size_t)(dot - base) : strlen(base);

	char *text = read_beside(path, base, base_len, ext);
	if (!text && title && title[0]) {
		text = read_beside(path, title, strlen(title), ext);
	}
	return text;
}

// ---------------------------------------------------------------------------
// LRC
// ---------------------------------------------------------------------------

// "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx", in milliseconds.
static bool parse_time(const char *t, size_t n, int32_t *ms) {
	size_t i = 0;
	long minutes = 0;
	if (i >= n || !isdigit((unsigned char)t[i])) {
		return false;
	}
	while (i < n && isdigit((unsigned char)t[i])) {
		minutes = minutes * 10 + (t[i++] - '0');
	}
	if (i >= n || t[i++] != ':' || i >= n || !isdigit((unsigned char)t[i])) {
		return false;
	}
	long seconds = 0;
	while (i < n && isdigit((unsigned char)t[i])) {
		seconds = seconds * 10 + (t[i++] - '0');
	}
	long fraction = 0;
	if (i < n && (t[i] == '.' || t[i] == ':')) {
		i++;
		long scale = 100;
		while (i < n && isdigit((unsigned char)t[i])) {
			fraction += (t[i++] - '0') * scale;
			scale /= 10;
		}
	}
	if (i != n) {
		return false;
	}
	*ms = (int32_t)(minutes * 60000 + seconds * 1000 + fraction);
	return true;
}

static bool line_has_time(const char *line) {
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	if (*line != '[') {
		return false;
	}
	const char *end = strchr(line, ']');
	int32_t ms;
	return end && parse_time(line + 1, (size_t)(end - line - 1), &ms);
}

// Word stamps from enhanced LRC ("<00:12.34>") taken out of the text, in place.
static void strip_word_stamps(char *text) {
	char *w = text;
	for (char *r = text; *r;) {
		if (*r == '<') {
			char *end = strchr(r, '>');
			int32_t ms;
			if (end && parse_time(r + 1, (size_t)(end - r - 1), &ms)) {
				r = end + 1;
				continue;
			}
		}
		*w++ = *r++;
	}
	*w = '\0';
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
		s[--n] = '\0';
	}
	return s;
}

typedef struct {
	lyrics_line_t line;
	int order;
} sortable_t;

static int by_time(const void *a, const void *b) {
	const sortable_t *x = a, *y = b;
	if (x->line.ms != y->line.ms) {
		return x->line.ms < y->line.ms ? -1 : 1;
	}
	return x->order - y->order;
}

static bool add_line(sortable_t **lines, int *count, int *cap, int32_t ms, const char *text) {
	if (*count >= LYRICS_LINES_MAX) {
		return false;
	}
	if (*count == *cap) {
		int grown = *cap ? *cap * 2 : 64;
		sortable_t *bigger = realloc(*lines, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		*lines = bigger;
		*cap = grown;
	}
	(*lines)[*count].line.ms = ms;
	(*lines)[*count].line.text = text;
	(*lines)[*count].order = *count;
	(*count)++;
	return true;
}

// Splits `text` (kept as the arena) into lines.
static bool parse(char *text, lyrics_t *out) {
	bool synced = false;
	for (char *p = text; *p;) {
		char *nl = strchr(p, '\n');
		if (nl) {
			*nl = '\0';
		}
		if (!synced && line_has_time(p)) {
			synced = true;
		}
		if (nl) {
			*nl = '\n';
			p = nl + 1;
		} else {
			break;
		}
	}

	sortable_t *lines = NULL;
	int count = 0;
	int cap = 0;
	int32_t offset = 0;
	bool blank_pending = false;

	char *p = text;
	while (p && *p) {
		char *nl = strchr(p, '\n');
		if (nl) {
			*nl = '\0';
		}
		char *next = nl ? nl + 1 : NULL;
		char *line = trim(p);

		// The leading tags: times, the offset, and the header tags ([ar:] and
		// the like) that name the song rather than being part of it.
		int32_t times[16];
		int time_count = 0;
		bool header = false;
		while (*line == '[') {
			char *end = strchr(line, ']');
			if (!end) {
				break;
			}
			int32_t ms;
			if (parse_time(line + 1, (size_t)(end - line - 1), &ms)) {
				if (time_count < 16) {
					times[time_count++] = ms;
				}
			} else if (strncmp(line + 1, "offset:", 7) == 0) {
				offset = (int32_t)strtol(line + 8, NULL, 10);
				header = true;
			} else if (isalpha((unsigned char)line[1]) && memchr(line + 1, ':', (size_t)(end - line - 1))) {
				header = true;
			} else {
				break;
			}
			line = end + 1;
		}
		line = trim(line);
		strip_word_stamps(line);

		if (synced) {
			for (int i = 0; i < time_count; i++) {
				add_line(&lines, &count, &cap, times[i], line);
			}
		} else if (!header || line[0]) {
			// Untimed: kept as written, one empty line between verses at most,
			// none at the ends.
			if (!line[0]) {
				blank_pending = count > 0;
			} else {
				if (blank_pending) {
					add_line(&lines, &count, &cap, -1, "");
					blank_pending = false;
				}
				add_line(&lines, &count, &cap, -1, line);
			}
		}
		p = next;
	}

	if (count == 0) {
		free(lines);
		return false;
	}

	if (synced) {
		qsort(lines, (size_t)count, sizeof(*lines), by_time);
		for (int i = 0; i < count; i++) {
			lines[i].line.ms -= offset; // a positive offset shows the words sooner
			if (lines[i].line.ms < 0) {
				lines[i].line.ms = 0;
			}
		}
	}

	out->lines = malloc((size_t)count * sizeof(*out->lines));
	if (!out->lines) {
		free(lines);
		return false;
	}
	for (int i = 0; i < count; i++) {
		out->lines[i] = lines[i].line;
	}
	free(lines);
	out->count = count;
	out->synced = synced;
	return true;
}

// ---------------------------------------------------------------------------

bool lyrics_load(const char *path, const char *title, lyrics_t *out) {
	memset(out, 0, sizeof(*out));
	if (!path || !path[0]) {
		return false;
	}

	char *text = read_by_names(path, title, "lrc");
	if (!text) {
		text = metadata_read_lyrics(path);
		if (text) {
			// Tags are already UTF-8; only a stray BOM is taken off.
			if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
				memmove(text, text + 3, strlen(text + 3) + 1);
			}
		}
	}
	if (!text) {
		text = read_by_names(path, title, "txt");
	}
	if (!text) {
		return false;
	}

	out->arena = text;
	if (!parse(text, out)) {
		lyrics_free(out);
		return false;
	}
	return true;
}

void lyrics_free(lyrics_t *lyrics) {
	if (!lyrics) {
		return;
	}
	free(lyrics->lines);
	free(lyrics->arena);
	memset(lyrics, 0, sizeof(*lyrics));
}

int lyrics_line_at(const lyrics_t *lyrics, int32_t ms) {
	if (!lyrics || !lyrics->synced || lyrics->count == 0 || ms < lyrics->lines[0].ms) {
		return -1;
	}
	int lo = 0;
	int hi = lyrics->count - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (lyrics->lines[mid].ms <= ms) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}
