// Headless checks of the Screensaver folder: which files count as pictures,
// decided from their JPEG headers, and the random pick between them.
//
// The pictures are synthetic. Only the headers matter here, so each file has a
// JPEG's segment structure, with the frame header where an editor would put
// it, and filler where the picture data would be. No decoder is linked.

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/screensaverpics.h"
#include "src/system/library/jpeginfo.h"

/* ---- synthetic JPEGs ------------------------------------------------------ */

typedef struct {
	unsigned char *data;
	size_t size;
	size_t cap;
} buf_t;

static void put_byte(buf_t *b, int value) {
	if (b->size == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 1024;
		b->data = realloc(b->data, b->cap);
		assert(b->data);
	}
	b->data[b->size++] = (unsigned char)value;
}

static void put_u16(buf_t *b, int value) {
	put_byte(b, (value >> 8) & 0xFF);
	put_byte(b, value & 0xFF);
}

// A segment of `payload` bytes after its length. The filler is a fake JPEG of
// its own, as an Exif thumbnail is: SOI and a frame header for a 160x120
// landscape picture, over and over. A reader that looked inside a segment
// instead of stepping over it would find that frame first and judge the file
// by it.
static void put_segment(buf_t *b, int marker, size_t payload) {
	static const unsigned char thumb[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x78, 0x00, 0xA0, 0x03};
	assert(payload + 2 <= 0xFFFF);
	put_byte(b, 0xFF);
	put_byte(b, marker);
	put_u16(b, (int)payload + 2);
	for (size_t i = 0; i < payload; i++) {
		put_byte(b, thumb[i % sizeof(thumb)]);
	}
}

// SOFn for a three-component picture.
static void put_frame(buf_t *b, int marker, int w, int h) {
	put_byte(b, 0xFF);
	put_byte(b, marker);
	put_u16(b, 8 + 3 * 3);
	put_byte(b, 8); // precision
	put_u16(b, h);
	put_u16(b, w);
	put_byte(b, 3);
	for (int c = 1; c <= 3; c++) {
		put_byte(b, c);
		put_byte(b, c == 1 ? 0x22 : 0x11);
		put_byte(b, c == 1 ? 0 : 1);
	}
}

// How much sits in front of the frame header.
typedef enum {
	META_NONE,	 // JFIF and straight on, as a stripped export
	META_EDITOR, // what a phone or an editor keeps: about 93 KB
} meta_t;

// Where the last make_jpeg() put its frame header. Searching the bytes for it
// would not do: the filler holds frame markers of its own.
static size_t last_frame_at;

// A JPEG's headers: JFIF, then for META_EDITOR the Exif block with its
// thumbnail, a colour profile, XMP and Photoshop's resources. Then the tables,
// the frame header, the scan and the end.
static buf_t make_jpeg(int w, int h, bool progressive, meta_t meta) {
	buf_t b = {0};
	put_byte(&b, 0xFF);
	put_byte(&b, 0xD8);
	put_segment(&b, 0xE0, 14); // JFIF
	if (meta == META_EDITOR) {
		put_segment(&b, 0xE1, 60000); // Exif, with its thumbnail
		put_segment(&b, 0xE2, 3144);  // an sRGB profile
		put_segment(&b, 0xE1, 20000); // XMP
		put_segment(&b, 0xED, 10000); // Photoshop
	}
	put_segment(&b, 0xDB, 67); // quantisation table
	last_frame_at = b.size;
	put_frame(&b, progressive ? 0xC2 : 0xC0, w, h);
	put_segment(&b, 0xC4, 31); // Huffman table
	put_segment(&b, 0xDA, 10); // the scan header, and then its data
	for (int i = 0; i < 256; i++) {
		put_byte(&b, i == 0xFF ? 0x00 : i);
	}
	put_byte(&b, 0xFF);
	put_byte(&b, 0xD9);
	return b;
}

// The "._" file a Mac writes beside each file it copies to a FAT card: an
// AppleDouble header, named like the picture.
static const unsigned char apple_double[] = {0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00, 'M', 'a', 'c', ' ', 'O', 'S', ' ', 'X'};

static void write_bytes(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, size, f) == size);
	assert(fclose(f) == 0);
}

static void write_jpeg(const char *dir, const char *name, int w, int h, bool progressive, meta_t meta) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	buf_t b = make_jpeg(w, h, progressive, meta);
	write_bytes(path, b.data, b.size);
	free(b.data);
}

static void remove_tree(const char *path) {
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
				continue;
			}
			char child[1024];
			snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
			struct stat st;
			if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
				remove_tree(child);
			} else {
				unlink(child);
			}
		}
		closedir(d);
	}
	rmdir(path);
}

/* ---- the headers ---------------------------------------------------------- */

static void check_headers(const char *tmp) {
	jpeg_info_t info;
	char path[1024];

	// JFIF and straight to the frame.
	buf_t plain = make_jpeg(240, 320, false, META_NONE);
	size_t frame_at = last_frame_at;
	assert(jpeg_info_parse(plain.data, plain.size, &info));
	assert(info.width == 240 && info.height == 320 && !info.progressive);
	snprintf(path, sizeof(path), "%s/plain.jpg", tmp);
	write_bytes(path, plain.data, plain.size);
	assert(jpeg_info_read(path, &info));
	assert(info.width == 240 && info.height == 320 && !info.progressive);

	// The frame behind 93 KB of an editor's metadata, read from the file. A
	// reader that stopped after the first 8 KB turned every such picture away,
	// whatever its size.
	buf_t heavy = make_jpeg(480, 800, false, META_EDITOR);
	assert(heavy.size > 90000);
	snprintf(path, sizeof(path), "%s/heavy.jpg", tmp);
	write_bytes(path, heavy.data, heavy.size);
	assert(jpeg_info_read(path, &info));
	assert(info.width == 480 && info.height == 800 && !info.progressive);
	assert(jpeg_info_parse(heavy.data, heavy.size, &info));
	assert(info.width == 480 && info.height == 800 && !info.progressive);

	// Ending inside the Exif block, or just after the frame marker: there is
	// no size to give.
	snprintf(path, sizeof(path), "%s/cut.jpg", tmp);
	write_bytes(path, heavy.data, 30000);
	assert(!jpeg_info_read(path, &info));
	assert(!jpeg_info_parse(heavy.data, 30000, &info));
	assert(plain.data[frame_at] == 0xFF && plain.data[frame_at + 1] == 0xC0);
	assert(!jpeg_info_parse(plain.data, frame_at + 4, &info));
	write_bytes(path, plain.data, frame_at + 4);
	assert(!jpeg_info_read(path, &info));

	// Progressive, behind the same metadata.
	buf_t prog = make_jpeg(480, 800, true, META_EDITOR);
	snprintf(path, sizeof(path), "%s/prog.jpg", tmp);
	write_bytes(path, prog.data, prog.size);
	assert(jpeg_info_read(path, &info));
	assert(info.width == 480 && info.height == 800 && info.progressive);

	// Fill bytes: any number of 0xFF may come before a marker.
	buf_t fill = {0};
	put_byte(&fill, 0xFF);
	put_byte(&fill, 0xD8);
	put_byte(&fill, 0xFF);
	put_byte(&fill, 0xFF);
	put_segment(&fill, 0xE0, 14);
	put_byte(&fill, 0xFF);
	put_frame(&fill, 0xC0, 300, 400);
	assert(jpeg_info_parse(fill.data, fill.size, &info));
	assert(info.width == 300 && info.height == 400);

	// A scan with no frame before it.
	buf_t no_frame = {0};
	put_byte(&no_frame, 0xFF);
	put_byte(&no_frame, 0xD8);
	put_segment(&no_frame, 0xDB, 67);
	put_segment(&no_frame, 0xDA, 10);
	assert(!jpeg_info_parse(no_frame.data, no_frame.size, &info));

	// Not a JPEG at all, and nothing to read.
	assert(!jpeg_info_parse(apple_double, sizeof(apple_double), &info));
	snprintf(path, sizeof(path), "%s/._plain.jpg", tmp);
	write_bytes(path, apple_double, sizeof(apple_double));
	assert(!jpeg_info_read(path, &info));
	snprintf(path, sizeof(path), "%s/missing.jpg", tmp);
	assert(!jpeg_info_read(path, &info));
	assert(!jpeg_info_parse(plain.data, 0, &info));

	// The two extensions, in either case, and nothing else.
	assert(jpeg_info_has_extension("a.jpg") && jpeg_info_has_extension("a.JPG"));
	assert(jpeg_info_has_extension("a.jpeg") && jpeg_info_has_extension("a.Jpeg"));
	assert(!jpeg_info_has_extension("a.png") && !jpeg_info_has_extension("jpg"));
	assert(!jpeg_info_has_extension("a.jpg.txt") && !jpeg_info_has_extension(NULL));

	free(plain.data);
	free(heavy.data);
	free(prog.data);
	free(fill.data);
	free(no_frame.data);
}

/* ---- the folder ----------------------------------------------------------- */

static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	assert(slash);
	return slash + 1;
}

static void check_folder(const char *tmp) {
	// Each a size that provably fits inside the next, as -Wformat-truncation
	// wants: the card in the folder's path, the folder in a file's.
	char card[256], folder[512], out[1024], path[1024];
	snprintf(card, sizeof(card), "%s/card", tmp);
	assert(mkdir(card, 0755) == 0);

	// No card: nothing to read.
	screensaverpics_set_root(NULL);
	assert(screensaverpics_dir()[0] == '\0');
	assert(!screensaverpics_any(480, 800));
	assert(!screensaverpics_pick(NULL, 480, 800, out, sizeof(out)) && out[0] == '\0');

	// A card, first with no folder and then with an empty one.
	screensaverpics_set_root(card);
	snprintf(folder, sizeof(folder), "%s/Screensaver", card);
	assert(strcmp(screensaverpics_dir(), folder) == 0);
	assert(!screensaverpics_any(480, 800));
	assert(mkdir(folder, 0755) == 0);
	assert(!screensaverpics_any(480, 800));
	assert(!screensaverpics_pick(NULL, 480, 800, out, sizeof(out)));

	// Two pictures made to the note the V1 shows (portrait, baseline, up to
	// 480x800), saved by an editor that keeps its metadata...
	write_jpeg(folder, "phone.JPG", 480, 800, false, META_EDITOR);
	write_jpeg(folder, "wallpaper.jpeg", 480, 640, false, META_EDITOR);
	// ...and everything the rules turn away.
	write_jpeg(folder, "wide.jpg", 800, 480, false, META_NONE);	   // landscape
	write_jpeg(folder, "big.jpg", 481, 800, false, META_NONE);	   // too wide
	write_jpeg(folder, "tall.jpg", 480, 801, false, META_NONE);	   // too tall
	write_jpeg(folder, "prog.jpg", 480, 800, true, META_NONE);	   // progressive
	write_jpeg(folder, ".hidden.jpg", 240, 320, false, META_NONE); // hidden, though fine
	write_jpeg(folder, "cover.png", 240, 320, false, META_NONE);   // not a JPEG's name
	snprintf(path, sizeof(path), "%s/._phone.JPG", folder);
	write_bytes(path, apple_double, sizeof(apple_double));
	snprintf(path, sizeof(path), "%s/notes.txt", folder);
	write_bytes(path, "notes", 5);

	assert(screensaverpics_usable(folder, "phone.JPG", 480, 800));
	assert(screensaverpics_usable(folder, "wallpaper.jpeg", 480, 800));
	assert(!screensaverpics_usable(folder, "wide.jpg", 800, 800)); // landscape even where it fits
	assert(!screensaverpics_usable(folder, "big.jpg", 480, 800));
	assert(!screensaverpics_usable(folder, "tall.jpg", 480, 800));
	assert(!screensaverpics_usable(folder, "prog.jpg", 480, 800));
	assert(!screensaverpics_usable(folder, ".hidden.jpg", 480, 800));
	assert(!screensaverpics_usable(folder, "._phone.JPG", 480, 800));
	assert(!screensaverpics_usable(folder, "cover.png", 480, 800));
	assert(!screensaverpics_usable(folder, "missing.jpg", 480, 800));
	assert(!screensaverpics_usable("", "phone.JPG", 480, 800));

	// The V1's fault: with its 240x320 panel as the limit, none of these
	// counted, and "No pictures to show" came up over a folder of pictures
	// made exactly to the note.
	assert(!screensaverpics_any(240, 320));
	assert(screensaverpics_any(480, 800));

	// The pick: only ever a usable picture, and over many wakes both.
	bool seen_phone = false, seen_wallpaper = false;
	for (int i = 0; i < 200; i++) {
		assert(screensaverpics_pick(NULL, 480, 800, out, sizeof(out)));
		assert(strncmp(out, folder, strlen(folder)) == 0 && out[strlen(folder)] == '/');
		const char *name = base_name(out);
		if (strcmp(name, "phone.JPG") == 0) {
			seen_phone = true;
		} else {
			assert(strcmp(name, "wallpaper.jpeg") == 0);
			seen_wallpaper = true;
		}
	}
	assert(seen_phone && seen_wallpaper);

	// A different one from the picture up, while there is one...
	snprintf(path, sizeof(path), "%s/phone.JPG", folder);
	for (int i = 0; i < 50; i++) {
		assert(screensaverpics_pick(path, 480, 800, out, sizeof(out)));
		assert(strcmp(base_name(out), "wallpaper.jpeg") == 0);
	}
	// ...and the same one again when it is the only one. A bare name will do.
	snprintf(path, sizeof(path), "%s/wallpaper.jpeg", folder);
	assert(unlink(path) == 0);
	for (int i = 0; i < 20; i++) {
		assert(screensaverpics_pick("phone.JPG", 480, 800, out, sizeof(out)));
		assert(strcmp(base_name(out), "phone.JPG") == 0);
	}

	// Hidden files never spend the pick's tries. With twenty of a Mac's "._"
	// files beside the one picture, each drawn like a picture, eight tries
	// would miss the picture about two times in three.
	for (int i = 0; i < 20; i++) {
		snprintf(path, sizeof(path), "%s/._extra%02d.jpg", folder, i);
		write_bytes(path, apple_double, sizeof(apple_double));
	}
	for (int i = 0; i < 100; i++) {
		assert(screensaverpics_pick(NULL, 480, 800, out, sizeof(out)));
		assert(strcmp(base_name(out), "phone.JPG") == 0);
	}

	screensaverpics_set_root(NULL);
}

int main(void) {
	char tmp[] = "/tmp/morpthis-pictures-test-XXXXXX";
	assert(mkdtemp(tmp));
	check_headers(tmp);
	check_folder(tmp);
	remove_tree(tmp);
	printf("PASS screensaver folder rules (headers behind large metadata, size limit, hidden files)\n");
	return 0;
}
