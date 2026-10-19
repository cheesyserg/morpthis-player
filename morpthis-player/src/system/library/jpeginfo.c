#include "jpeginfo.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> // strcasecmp

// Start Of Frame markers. The progressive ones are the second column: SOF2 and
// its arithmetic and hierarchical relatives, all of which the scaled decoder
// refuses. Everything else in the C0..CF block that is not a frame header --
// DHT (C4), JPG (C8), DAC (CC) -- is skipped like an ordinary segment.
static bool is_frame_marker(unsigned char m) {
	if (m == 0xC4 || m == 0xC8 || m == 0xCC) {
		return false;
	}
	return m >= 0xC0 && m <= 0xCF;
}

static bool is_progressive_marker(unsigned char m) {
	return m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
}

// The headers are walked one segment at a time, from bytes in hand or straight
// from the file. Every segment that is not the frame header is stepped over by
// its length, never read.
//
// Nothing limits how far into the file the frame header may sit. What comes
// before it is metadata: Exif with its thumbnail, colour profiles, XMP and
// Photoshop's resources. A phone, Photoshop or GIMP keeping all of that puts
// the frame header tens or hundreds of kilobytes in. A reader that gave up
// after the first 8 KB turned every one of those pictures away, whatever its
// size. Stepping over a segment by seeking costs the same at 60 KB as at 60
// bytes.
typedef struct {
	const unsigned char *data; // bytes in hand, or NULL to read `file`
	size_t size;
	size_t pos;
	FILE *file;
} cursor_t;

// The next byte, or -1 at the end.
static int next_byte(cursor_t *c) {
	if (!c->data) {
		return fgetc(c->file); // EOF is -1
	}
	return c->pos < c->size ? c->data[c->pos++] : -1;
}

// Steps over `count` bytes, which a segment length keeps under 64 KB. False
// when that runs past the end of bytes in hand. A file may seek past its end;
// the read after that is what says the file ended too soon.
static bool skip_bytes(cursor_t *c, size_t count) {
	if (!c->data) {
		return fseek(c->file, (long)count, SEEK_CUR) == 0;
	}
	if (count > c->size - c->pos) {
		return false;
	}
	c->pos += count;
	return true;
}

static bool walk_headers(cursor_t *c, jpeg_info_t *out) {
	if (next_byte(c) != 0xFF || next_byte(c) != 0xD8) {
		return false; // no SOI: not a JPEG
	}

	for (;;) {
		// Segments are separated by fill bytes in some encoders' output: any
		// number of 0xFF may precede the marker itself.
		int b = next_byte(c);
		if (b != 0xFF) {
			return false; // out of step with the segment structure, or the end
		}
		while (b == 0xFF) {
			b = next_byte(c);
		}
		if (b < 0) {
			return false;
		}
		unsigned char marker = (unsigned char)b;

		// The standalone markers carry no length: restart intervals, SOI, EOI
		// and TEM. Nothing follows them to skip.
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
			continue;
		}
		if (marker == 0xDA) {
			return false; // the scan begins and no frame header was found
		}

		int hi = next_byte(c);
		int lo = next_byte(c);
		if (hi < 0 || lo < 0) {
			return false;
		}
		unsigned int length = ((unsigned int)hi << 8) | (unsigned int)lo;
		if (length < 2) {
			return false;
		}

		if (is_frame_marker(marker)) {
			// SOFn payload after the length: precision(1), height(2), width(2).
			if (length < 7) {
				return false;
			}
			int field[5];
			for (int i = 0; i < 5; i++) {
				field[i] = next_byte(c);
				if (field[i] < 0) {
					return false;
				}
			}
			out->height = (field[1] << 8) | field[2];
			out->width = (field[3] << 8) | field[4];
			out->progressive = is_progressive_marker(marker);
			return out->width > 0 && out->height > 0;
		}

		if (!skip_bytes(c, length - 2)) { // the length counts itself
			return false;
		}
	}
}

bool jpeg_info_parse(const unsigned char *data, size_t size, jpeg_info_t *out) {
	if (!data || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	cursor_t c = {.data = data, .size = size};
	return walk_headers(&c, out);
}

bool jpeg_info_read(const char *path, jpeg_info_t *out) {
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	if (!path || !path[0] || !out) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	cursor_t c = {.file = f};
	bool ok = walk_headers(&c, out);
	fclose(f);
	return ok;
}

bool jpeg_info_has_extension(const char *name) {
	if (!name) {
		return false;
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}
