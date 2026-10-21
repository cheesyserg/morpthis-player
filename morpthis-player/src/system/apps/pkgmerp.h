#ifndef PKGMERP_H
#define PKGMERP_H

#include <stdbool.h>
#include <stddef.h>

// .pkgmerp packages.
//
// An app package is a single uncompressed squashfs image. The player mounts it
// read-only through a loop device and runs the entry from the mount point, so
// nothing has to be extracted to the card first and the image stays the one
// file a user copies or deletes.
//
// Uncompressed is deliberate. The kernel serves file data straight from the
// card through the page cache; a compressed image would make it decompress
// into a workspace first, which is RAM this player does not have to spare.
// The trade is image size on the card, where there is room.
//
// Mount points live under /tmp/morpthis/pkgmerp/<id>: a directory there costs
// nothing of consequence, and it disappears with a reboot even if a run left
// something behind. The data an app writes is not here; apps keep it in
// <card>/merp_data/<id>, which is a real writable folder on the card.

#define PKGMERP_EXT ".pkgmerp"
#define PKGMERP_DIR "pkgmerps"
#define PKGMERP_MOUNT_ROOT "/tmp/morpthis/pkgmerp"

// The mount point for a package id, e.g. /tmp/morpthis/pkgmerp/calculator.
// The id is reduced to a safe file name. False when it is empty or too long.
bool pkgmerp_mount_point(const char *id, char *out, size_t size);

// True when the file starts with a squashfs superblock. Checks the magic only;
// the mount is what proves the rest.
bool pkgmerp_looks_like_image(const char *path, char *error, size_t error_size);

// Mounts `image` read-only on `mount_point`, allocating a loop device. A
// package that is already mounted by this process is shared: pkgmerp_unmount()
// must be called once per successful mount, and the last call releases the
// loop device. On failure `error` says why and no loop device is left behind.
bool pkgmerp_mount(const char *image, const char *mount_point, char *error,
				   size_t error_size);

// Unmounts a mount made by pkgmerp_mount() and releases its loop device.
// Returns true when the path is not mounted by this process.
bool pkgmerp_unmount(const char *mount_point, char *error, size_t error_size);

// Whether pkgmerp_mount() mounted `mount_point` and it has not been released.
bool pkgmerp_is_mounted(const char *mount_point);

#endif /* PKGMERP_H */
