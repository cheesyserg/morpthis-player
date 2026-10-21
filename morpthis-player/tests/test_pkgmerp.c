// Regression test for the pkgmerp mount registry.
//
// The bug this guards against: the "find a free slot" lookup also compared the
// mount point, and a free slot has an empty path, so the very first package
// mount reported "too many mounted packages". The real pkgmerp.c is linked;
// only the loop/mount syscalls are wrapped.

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/apps/pkgmerp.h"

// ---------------------------------------------------------------------------
// Wrapped syscalls: no root, no loop devices, no real mounts
// ---------------------------------------------------------------------------

int __real_open(const char *path, int flags, ...);
int __real_open64(const char *path, int flags, ...);

static int wrap_open_common(const char *path, int flags,
							int (*real_open)(const char *, int, ...)) {
	if (strcmp(path, "/dev/loop-control") == 0) {
		errno = ENOENT;
		return -1;
	}
	if (strncmp(path, "/dev/loop", 9) == 0) {
		return real_open("/dev/null", O_RDWR);
	}
	return real_open(path, flags);
}

int __wrap_open(const char *path, int flags, ...) {
	return wrap_open_common(path, flags, __real_open);
}

// _FILE_OFFSET_BITS=64 maps open() to open64() on glibc, so both symbols need
// a wrapper for the real pkgmerp.c to see the fake loop devices.
int __wrap_open64(const char *path, int flags, ...) {
	return wrap_open_common(path, flags, __real_open64);
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
	(void)fd;
	(void)request;
	return 0;
}

int __wrap_mount(const char *source, const char *target, const char *filesystemtype,
				 unsigned long mountflags, const void *data) {
	(void)source;
	(void)target;
	(void)filesystemtype;
	(void)mountflags;
	(void)data;
	return 0;
}

int __wrap_umount(const char *target) {
	(void)target;
	return 0;
}

int __wrap_umount2(const char *target, int flags) {
	(void)target;
	(void)flags;
	return 0;
}

int __wrap_mkdir(const char *path, mode_t mode) {
	(void)path;
	(void)mode;
	return 0;
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

static int failures;
static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static bool write_image(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	static const unsigned char image[4096] = {'h', 's', 'q', 's'};
	bool ok = fwrite(image, 1, sizeof(image), f) == sizeof(image);
	fclose(f);
	return ok;
}

int main(void) {
	char image[256];
	snprintf(image, sizeof(image), "/tmp/pkgmerp-registry-%ld.pkgmerp", (long)getpid());
	check(write_image(image), "write the fake image");

	char error[128] = "";

	// Six sequential mounts: the old lookup failed on the first one.
	for (int i = 0; i < 6; i++) {
		char point[64];
		snprintf(point, sizeof(point), "/tmp/pkgmerp-registry/%d", i);
		check(pkgmerp_mount(image, point, error, sizeof(error)), "sequential mount");
		check(pkgmerp_is_mounted(point), "sequential mount is registered");
		check(pkgmerp_unmount(point, error, sizeof(error)), "sequential unmount");
		check(!pkgmerp_is_mounted(point), "sequential unmount is released");
	}

	// The table holds four mounts at once. The fifth is the only refusal.
	char points[4][64];
	for (int i = 0; i < 4; i++) {
		snprintf(points[i], sizeof(points[i]), "/tmp/pkgmerp-registry/held%d", i);
		check(pkgmerp_mount(image, points[i], error, sizeof(error)), "four simultaneous mounts");
	}
	check(!pkgmerp_mount(image, "/tmp/pkgmerp-registry/fifth", error, sizeof(error)),
		  "the fifth simultaneous mount is refused");
	check(strcmp(error, "too many mounted packages") == 0, "the refusal names the slot limit");
	for (int i = 0; i < 4; i++) {
		check(pkgmerp_unmount(points[i], error, sizeof(error)), "release a held mount");
	}

	// A shared mount counts references; only the last unmount releases it.
	check(pkgmerp_mount(image, points[0], error, sizeof(error)), "first reference");
	check(pkgmerp_mount(image, points[0], error, sizeof(error)), "second reference");
	check(pkgmerp_unmount(points[0], error, sizeof(error)), "drop one reference");
	check(pkgmerp_is_mounted(points[0]), "the shared mount survives one release");
	check(pkgmerp_unmount(points[0], error, sizeof(error)), "drop the last reference");
	check(!pkgmerp_is_mounted(points[0]), "the shared mount is gone after the last release");

	remove(image);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_pkgmerp: mount registry ok\n");
	return 0;
}
