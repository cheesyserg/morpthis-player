#include "textfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // fsync, fileno

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------
//
// Moved here from the file manager's text viewer, which needed it first and
// had it to itself. The rules are exactly what that page reads files by, so a
// note and a .txt opened from the browser come out the same.

// Windows-1252 from 0x80 to 0x9F; 0 where the code page leaves a hole. From
// 0xA0 up it is Latin-1, the same numbers as Unicode.
static const uint16_t CP1252_HIGH[32] = {
	0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
	0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
	0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Appends code point `cp` as UTF-8. Returns the bytes written.
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

// The length of the valid UTF-8 sequence at `p`, 0 when there is none. A
// sequence cut by the end of the buffer counts as valid: the file was read up
// to a limit, not to its end.
static size_t utf8_len(const unsigned char *p, size_t left) {
	size_t need = p[0] < 0x80 ? 1 : (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 0;
	if (need == 0 || (need == 2 && p[0] < 0xC2)) {
		return 0;
	}
	for (size_t i = 1; i < need; i++) {
		if (i >= left) {
			return left;
		}
		if ((p[i] & 0xC0) != 0x80) {
			return 0;
		}
	}
	return need;
}

static bool is_utf8(const unsigned char *p, size_t len) {
	for (size_t i = 0; i < len;) {
		size_t n = utf8_len(p + i, len - i);
		if (n == 0) {
			return false;
		}
		i += n;
	}
	return true;
}

// Adds one code point to `out`, with line endings made "\n", tabs made four
// spaces and other control characters left out.
static size_t put_char(char *out, uint32_t cp, uint32_t *prev) {
	uint32_t before = *prev;
	*prev = cp;
	if (cp == '\r') {
		out[0] = '\n';
		return 1;
	}
	if (cp == '\n') {
		if (before == '\r') {
			return 0; // the second half of "\r\n"
		}
		out[0] = '\n';
		return 1;
	}
	if (cp == '\t') {
		memcpy(out, "    ", 4);
		return 4;
	}
	if (cp < 0x20 || cp == 0x7F || cp == 0xFEFF) {
		return 0;
	}
	return put_utf8(out, cp);
}

char *textfile_decode(const unsigned char *raw, size_t len) {
	char *out = malloc(len * 4 + 1);
	if (!out) {
		return NULL;
	}
	size_t o = 0;
	uint32_t prev = 0;

	bool le = len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE;
	bool be = len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF;
	if (le || be) {
		for (size_t i = 2; i + 1 < len; i += 2) {
			uint32_t unit = le ? (uint32_t)(raw[i] | (raw[i + 1] << 8)) : (uint32_t)((raw[i] << 8) | raw[i + 1]);
			if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < len) {
				uint32_t low = le ? (uint32_t)(raw[i + 2] | (raw[i + 3] << 8))
								  : (uint32_t)((raw[i + 2] << 8) | raw[i + 3]);
				if (low >= 0xDC00 && low < 0xE000) {
					unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
					i += 2;
				}
			}
			if (unit >= 0xD800 && unit < 0xE000) {
				continue;
			}
			o += put_char(out + o, unit, &prev);
		}
	} else if (is_utf8(raw, len)) {
		for (size_t i = 0; i < len;) {
			size_t n = utf8_len(raw + i, len - i);
			if (raw[i] < 0x80) {
				o += put_char(out + o, raw[i], &prev);
			} else if (n == 3 && raw[i] == 0xEF && raw[i + 1] == 0xBB && raw[i + 2] == 0xBF) {
				prev = 0; // a byte order mark
			} else {
				memcpy(out + o, raw + i, n);
				o += n;
				prev = 0x80;
			}
			i += n;
		}
	} else {
		for (size_t i = 0; i < len; i++) {
			uint32_t cp = raw[i] < 0x80 ? raw[i] : raw[i] < 0xA0 ? CP1252_HIGH[raw[i] - 0x80] : raw[i];
			if (cp) {
				o += put_char(out + o, cp, &prev);
			}
		}
	}

	out[o] = '\0';
	return out;
}

// ---------------------------------------------------------------------------
// the file
// ---------------------------------------------------------------------------

bool textfile_read(const char *path, size_t max, unsigned char **raw, size_t *bytes, bool *truncated) {
	*raw = NULL;
	*bytes = 0;
	*truncated = false;

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char *buffer = malloc(max > 0 ? max : 1);
	if (!buffer) {
		fclose(f);
		return false;
	}
	size_t len = fread(buffer, 1, max, f);
	bool more = fgetc(f) != EOF;
	bool failed = ferror(f) != 0;
	fclose(f);
	if (failed) {
		free(buffer);
		return false;
	}

	*raw = buffer;
	*bytes = len;
	*truncated = more;
	return true;
}

bool textfile_write(const char *path, const char *text) {
	char temp[1024];
	int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
	if (n <= 0 || (size_t)n >= sizeof(temp)) {
		return false;
	}

	size_t len = text ? strlen(text) : 0;
	bool need_newline = len > 0 && text[len - 1] != '\n';

	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(text ? text : "", 1, len, f) == len;
	if (ok && need_newline) {
		ok = fputc('\n', f) != EOF;
	}
	// The bytes have to be on the card before the rename, or a card pulled out
	// in between leaves the new name pointing at an empty file -- which is
	// worse than not having written at all.
	if (ok) {
		ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	}
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(temp);
		return false;
	}
	if (rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}
