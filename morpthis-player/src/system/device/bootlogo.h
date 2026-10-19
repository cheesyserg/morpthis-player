#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#include <stdbool.h>

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
// Two boot screens fit the stock script's three branches: Stock follows the
// theme in the first field, and Retrospace is theme:3, the value that falls
// through to /etc/logo.jpeg. The marker this player writes is still
// "theme:N logo:M", fourteen bytes, so the second field records the choice for
// the boot trace the firmware packer leaves in /tmp; the stock script never
// looks past its own seven-byte read.
//
// Two things this module is careful about, because raw flash is not a file:
// it reads the marker first and does nothing when it already agrees (NAND has
// a finite number of erase cycles and a theme switch is one tap), and it does
// the writing on a thread of its own (flash_erase takes long enough to be seen
// as a freeze).

// The boot screens Appearance > Boot screen offers. Retrospace is the default,
// and it is also what a device with no marker at all draws, because the stock
// script's fallback file is the one that carries it.
#define BOOTLOGO_STOCK      0
#define BOOTLOGO_RETROSPACE 1

// Points the boot logo at the theme and at the chosen boot screen: the stock
// pair follows the palette, Retrospace is one picture either way.
// Returns immediately; the flash work happens on a worker. A no-op when the
// marker already agrees, when the device has no /dev/mtd5, or on the host
// build.
void bootlogo_set(int choice, bool dark);

// What the boot script left in /tmp this boot, and what the marker writer
// made of the flash: two machine-readable lines for the Developer options
// page, or NULL when there is nothing to show yet.
const char *bootlogo_trace(void);
const char *bootlogo_status(void);

#endif /* BOOTLOGO_H */
