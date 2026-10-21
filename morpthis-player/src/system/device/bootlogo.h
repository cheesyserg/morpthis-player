#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The picture shown while the player boots, and how it follows the theme.
//
// It is not the player that draws it: by the time the player is running the
// logo has long gone. It is `/etc/init.d/S11jpeg_display_shell`, which runs
// before anything else and picks one of three files:
//
//     theme=$(nanddump -q -s 0x20000 -l 7 /dev/mtd5 -a)
//     if   [ "$theme" == "theme:1" ]; then cmd_jpeg_display /etc/logo1.jpeg
//     elif [ "$theme" == "theme:2" ]; then cmd_jpeg_display /etc/logo2.jpeg
//     else                                 cmd_jpeg_display /etc/logo.jpeg
//     fi
//
// So the setting does not live in a file at all -- a rootfs mounted read-only
// could not hold it, and /usr/data is not mounted yet when the script runs. It
// lives in raw NAND, in a block of /dev/mtd5 that belongs to nothing else, and
// the stock player writes it with the two commands its own binary carries as
// strings:
//
//     flash_erase -q /dev/mtd5 0x20000 1
//     printf %-256s | nandwrite -q -s 0x20000 -p /dev/mtd5 -
//
// logo1 is the pale one and logo2 the dark one, which is why light theme
// writes 1 and dark writes 2.
//
// Three boot screens fit the stock script's three branches: Stock follows the
// theme in the first field, Retrospace is theme:3, the value that falls
// through to /etc/logo.jpeg, and Custom is also theme:3 but carries its own
// picture. The marker this player writes is still "theme:N logo:M", padded to
// 64 bytes, so the second field records the choice for the boot trace the
// firmware packer leaves in /tmp; the stock script never looks past its own
// seven-byte read.
//
// A custom picture cannot be a file the boot script reads: the rootfs is
// read-only and /usr/data is not mounted yet. It is therefore written into the
// same erase block as the marker, after the marker's page, and the packer's
// block in the boot script copies it out to /tmp with nanddump before drawing
// it. The marker carries the picture's length and hash so the player can tell
// whether the block already holds the picture it wants, and the boot script
// can tell whether there is anything to copy. The picture is also kept at
// BOOTLOGO_CUSTOM_IMAGE on the writable partition, which is what the Boot
// screen centre previews and what survives the card being removed.
//
// Two things this module is careful about, because raw flash is not a file:
// it reads the marker first and does nothing when it already agrees (NAND has
// a finite number of erase cycles and a theme switch is one tap), and it does
// the writing on a thread of its own (flash_erase takes long enough to be seen
// as a freeze).

// The boot screens the Boot screen centre offers. Retrospace is the default,
// and it is also what a device with no marker at all draws, because the stock
// script's fallback file is the one that carries it.
#define BOOTLOGO_STOCK      0
#define BOOTLOGO_RETROSPACE 1
#define BOOTLOGO_CUSTOM     2

// Where the applied custom picture lives on the writable partition. The boot
// script never reads this path -- it reads the copy in flash -- but the Boot
// screen centre does, and it is what makes the choice survive the card being
// pulled out.
#define BOOTLOGO_CUSTOM_IMAGE "/usr/data/bootscreen.jpg"

// The most a custom picture may be. The marker block is at least 64 KiB on
// this NAND, and the picture starts one page into it, so this leaves room for
// the marker and the page padding on either side.
#define BOOTLOGO_IMAGE_MAX (56 * 1024)

// Points the boot logo at the theme and at the chosen boot screen: the stock
// pair follows the palette, Retrospace is one picture either way, and Custom
// is the picture at BOOTLOGO_CUSTOM_IMAGE. Returns immediately; the flash work
// happens on a worker. A no-op when the marker already agrees, when the device
// has no /dev/mtd5, or on the host build. A custom choice whose picture is
// missing falls back to Retrospace.
void bootlogo_set(int choice, bool dark);

// The applied custom picture, as the Boot screen centre reads it. False when
// there is none, or when the file is larger than the flash copy can hold.
bool bootlogo_custom_available(void);
const char *bootlogo_custom_path(void);

// Replaces the applied custom picture. Writes the JPEG atomically to
// BOOTLOGO_CUSTOM_IMAGE; the caller then calls bootlogo_set(BOOTLOGO_CUSTOM,
// dark) to put it in flash. False when the picture is empty, too large, or
// cannot be written.
bool bootlogo_custom_store(const uint8_t *jpeg, size_t len);

// What the boot script left in /tmp this boot, and what the marker writer
// made of the flash: two machine-readable lines for the Developer options
// page, or NULL when there is nothing to show yet.
const char *bootlogo_trace(void);
const char *bootlogo_status(void);

#endif /* BOOTLOGO_H */
