#ifndef SYSTEM_TEXT_TEXTFILE_H
#define SYSTEM_TEXT_TEXTFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Text files off the card, read into UTF-8 that the interface can draw, and
// written back out as UTF-8.
//
// Two pages need this and neither owns it: the file manager's text viewer,
// which only reads, and the note editor, which reads and writes. Keeping it
// here also keeps it testable without LVGL (tests/test_notes.c covers the
// decoding and the folder rules).

// `raw`, `len` bytes of a text file, as a newly allocated UTF-8 string.
//
// UTF-8 (with or without a byte order mark) and UTF-16 with a byte order mark
// are read as such; anything else that is not valid UTF-8 is taken for
// Windows-1252, which is what a file saved on Windows as "ANSI" is. Line
// endings become "\n", tabs become four spaces, and other control characters
// are left out -- a NUL, an escape or a stray byte from a file saved as RTF
// would otherwise be drawn as a box or cut the line short.
//
// Every input byte becomes at most four output bytes, so a buffer of len * 4
// + 1 is always enough. NULL when there is no memory for it.
char *textfile_decode(const unsigned char *raw, size_t len);

// The whole of `path`, decoded, at most `max` bytes read. `raw` and `bytes`
// are filled with what was read and are owned by the caller; `*truncated` says
// the file was longer than `max`, and false is returned when it could not be
// read at all.
bool textfile_read(const char *path, size_t max, unsigned char **raw, size_t *bytes, bool *truncated);

// `text` written to `path` as UTF-8, with no byte order mark, LF line endings
// and a final newline. Written to a temporary beside it and renamed over, so a
// card pulled out mid-write leaves the old note rather than half a new one.
bool textfile_write(const char *path, const char *text);

#endif /* SYSTEM_TEXT_TEXTFILE_H */
