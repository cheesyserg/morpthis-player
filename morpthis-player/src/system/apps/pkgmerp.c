#include "pkgmerp.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Older kernel headers predate the loop-control node; the number is stable.
#ifndef LOOP_CTL_GET_FREE
#define LOOP_CTL_GET_FREE 0x4C82
#endif
#ifndef LO_FLAGS_READ_ONLY
#define LO_FLAGS_READ_ONLY 1
#endif
#ifndef LO_FLAGS_AUTOCLEAR
#define LO_FLAGS_AUTOCLEAR 4
#endif

#define PKGMERP_PATH_MAX 1024
#define PKGMERP_MAX_MOUNTS 4

// Only this process mounts app packages, and it runs one app at a time; the
// scan mounts one package at a time as well. Four slots leave room for a
// running app, a scan and a tool that was started while one of those was up.
typedef struct {
	bool used;
	int refs;
	char image[PKGMERP_PATH_MAX];
	char mount_point[PKGMERP_PATH_MAX];
	char loop_device[32];
	int loop_fd;
} mount_entry_t;

static mount_entry_t mounts[PKGMERP_MAX_MOUNTS];
static pthread_mutex_t mounts_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(char *out, size_t size, const char *format, ...) {
	if (!out || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(out, size, format, args);
	va_end(args);
}

// mkdir -p for the mount point. Every component under /tmp is made, existing
// ones are fine, and anything else is an error worth reporting.
static int make_directories(const char *path) {
	char work[PKGMERP_PATH_MAX];
	size_t length = strlen(path);
	if (length == 0 || length >= sizeof(work)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(work, path, length + 1);

	for (char *p = work + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(work, 0755) != 0 && errno != EEXIST) {
			return -1;
		}
		*p = '/';
	}

	if (mkdir(work, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

bool pkgmerp_mount_point(const char *id, char *out, size_t size) {
	if (!id || !id[0] || !out || size == 0) {
		return false;
	}

	char safe[64];
	size_t used = 0;
	for (const char *p = id; *p && used < sizeof(safe) - 1; p++) {
		unsigned char c = (unsigned char)*p;
		safe[used++] = (isalnum(c) || c == '.' || c == '_' || c == '-') ? (char)c : '_';
	}
	safe[used] = '\0';
	if (used == 0) {
		return false;
	}

	int n = snprintf(out, size, "%s/%s", PKGMERP_MOUNT_ROOT, safe);
	return n > 0 && (size_t)n < size;
}

bool pkgmerp_looks_like_image(const char *path, char *error, size_t error_size) {
	if (!path || !path[0]) {
		set_error(error, error_size, "no package file");
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		set_error(error, error_size, "%s", strerror(errno));
		return false;
	}

	unsigned char magic[4];
	size_t got = fread(magic, 1, sizeof(magic), f);
	fclose(f);

	// squashfs stores its 0x73717368 magic in the target's byte order, so a
	// little-endian image reads "hsqs" and a big-endian one "sqsh".
	if (got != sizeof(magic) ||
		(memcmp(magic, "hsqs", 4) != 0 && memcmp(magic, "sqsh", 4) != 0)) {
		set_error(error, error_size, "not a squashfs image");
		return false;
	}
	return true;
}

// Attaches `image_fd` to `device`. Returns the open loop descriptor, which
// must stay open for as long as the mount exists: LO_FLAGS_AUTOCLEAR detaches
// the device when the last descriptor is closed.
static int attach_to_loop(const char *device, int image_fd) {
	int loop_fd = open(device, O_RDWR | O_CLOEXEC);
	if (loop_fd < 0) {
		return -1;
	}
	if (ioctl(loop_fd, LOOP_SET_FD, image_fd) != 0) {
		close(loop_fd);
		return -1;
	}
	return loop_fd;
}

// Finds a free loop device and attaches the image to it.
static int attach_loop(const char *image, char *out_device, size_t out_size,
					   char *error, size_t error_size) {
	int image_fd = open(image, O_RDONLY | O_CLOEXEC);
	if (image_fd < 0) {
		set_error(error, error_size, "open: %s", strerror(errno));
		return -1;
	}

	// Ask the kernel first: it knows which loop devices are free, and the node
	// it names is the one to use.
	int preferred = -1;
	int control = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (control >= 0) {
		preferred = ioctl(control, LOOP_CTL_GET_FREE);
		close(control);
	}

	int loop_fd = -1;
	if (preferred >= 0) {
		snprintf(out_device, out_size, "/dev/loop%d", preferred);
		loop_fd = attach_to_loop(out_device, image_fd);
	}

	// No loop-control node, or the device it named was taken between the ioctl
	// and the open. The known nodes are few enough to try in order.
	for (int i = 0; loop_fd < 0 && i < 16; i++) {
		snprintf(out_device, out_size, "/dev/loop%d", i);
		loop_fd = attach_to_loop(out_device, image_fd);
	}

	close(image_fd);
	if (loop_fd < 0) {
		set_error(error, error_size, "no free loop device");
		return -1;
	}

	// Read-only at the device level, and detached when the last user lets go.
	// A kernel without LOOP_SET_STATUS64 still mounts: the read-only bind
	// comes from the mount flags below.
	struct loop_info64 info;
	memset(&info, 0, sizeof(info));
	info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
	if (ioctl(loop_fd, LOOP_SET_STATUS64, &info) != 0) {
		fprintf(stderr, "pkgmerp: %s: loop status: %s\n", out_device, strerror(errno));
	}

	return loop_fd;
}

static mount_entry_t *find_mount(const char *mount_point) {
	for (size_t i = 0; i < PKGMERP_MAX_MOUNTS; i++) {
		if (mounts[i].used && strcmp(mounts[i].mount_point, mount_point) == 0) {
			return &mounts[i];
		}
	}
	return NULL;
}

// A free slot has an empty mount_point, so it cannot be found by matching the
// path: the lookup is only about `used`.
static mount_entry_t *find_free(void) {
	for (size_t i = 0; i < PKGMERP_MAX_MOUNTS; i++) {
		if (!mounts[i].used) {
			return &mounts[i];
		}
	}
	return NULL;
}

bool pkgmerp_is_mounted(const char *mount_point) {
	if (!mount_point || !mount_point[0]) {
		return false;
	}
	pthread_mutex_lock(&mounts_lock);
	bool found = find_mount(mount_point) != NULL;
	pthread_mutex_unlock(&mounts_lock);
	return found;
}

bool pkgmerp_mount(const char *image, const char *mount_point, char *error,
				   size_t error_size) {
	if (!image || !image[0] || !mount_point || !mount_point[0]) {
		set_error(error, error_size, "no package or mount point");
		return false;
	}

	pthread_mutex_lock(&mounts_lock);

	mount_entry_t *mounted = find_mount(mount_point);
	if (mounted) {
		if (strcmp(mounted->image, image) != 0) {
			pthread_mutex_unlock(&mounts_lock);
			set_error(error, error_size, "mount point is already holding another package");
			return false;
		}
		mounted->refs++;
		pthread_mutex_unlock(&mounts_lock);
		return true;
	}

	mount_entry_t *slot = find_free();
	if (!slot) {
		pthread_mutex_unlock(&mounts_lock);
		set_error(error, error_size, "too many mounted packages");
		return false;
	}

	char why[128];
	if (!pkgmerp_looks_like_image(image, why, sizeof(why))) {
		pthread_mutex_unlock(&mounts_lock);
		set_error(error, error_size, "%s", why);
		return false;
	}

	if (make_directories(mount_point) != 0) {
		int err = errno;
		pthread_mutex_unlock(&mounts_lock);
		set_error(error, error_size, "mount point: %s", strerror(err));
		return false;
	}

	char loop_device[32];
	int loop_fd = attach_loop(image, loop_device, sizeof(loop_device), why, sizeof(why));
	if (loop_fd < 0) {
		pthread_mutex_unlock(&mounts_lock);
		set_error(error, error_size, "%s", why);
		return false;
	}

	if (mount(loop_device, mount_point, "squashfs", MS_RDONLY | MS_NOATIME, NULL) != 0) {
		int err = errno;
		ioctl(loop_fd, LOOP_CLR_FD, 0);
		close(loop_fd);
		pthread_mutex_unlock(&mounts_lock);
		set_error(error, error_size, "mount: %s", strerror(err));
		return false;
	}

	slot->used = true;
	slot->refs = 1;
	snprintf(slot->image, sizeof(slot->image), "%s", image);
	snprintf(slot->mount_point, sizeof(slot->mount_point), "%s", mount_point);
	snprintf(slot->loop_device, sizeof(slot->loop_device), "%s", loop_device);
	slot->loop_fd = loop_fd;

	printf("pkgmerp: mounted %s on %s (%s)\n", image, mount_point, loop_device);
	pthread_mutex_unlock(&mounts_lock);
	return true;
}

bool pkgmerp_unmount(const char *mount_point, char *error, size_t error_size) {
	if (!mount_point || !mount_point[0]) {
		return true;
	}

	pthread_mutex_lock(&mounts_lock);

	mount_entry_t *slot = find_mount(mount_point);
	if (!slot) {
		pthread_mutex_unlock(&mounts_lock); // not ours; nothing to release
		return true;
	}

	// One mount may be shared by a scan and a launch. The filesystem goes away
	// when the last caller lets go.
	if (--slot->refs > 0) {
		pthread_mutex_unlock(&mounts_lock);
		return true;
	}

	bool released = true;
	if (umount(mount_point) != 0 && errno != EINVAL) {
		// The app has been unloaded by now, so anything still holding the
		// mount is the kernel's business. A detached mount frees the name
		// immediately and cleans up when the last reference goes.
		if (umount2(mount_point, MNT_DETACH) != 0) {
			int err = errno;
			released = false;
			set_error(error, error_size, "umount: %s", strerror(err));
			fprintf(stderr, "pkgmerp: could not unmount %s: %s\n", mount_point, strerror(err));
		}
	}

	if (released) {
		// AUTOCLEAR would do this on close; doing it explicitly keeps a
		// failed status ioctl from leaking the device. A failure here is worth
		// saying out loud: a loop still attached keeps the card's block device
		// busy, and USB mass storage then cannot unmount the card either.
		if (ioctl(slot->loop_fd, LOOP_CLR_FD, 0) != 0) {
			fprintf(stderr, "pkgmerp: LOOP_CLR_FD %s failed: %s\n",
					slot->loop_device[0] ? slot->loop_device : "?", strerror(errno));
		}
		close(slot->loop_fd);
		slot->loop_fd = -1;
		slot->refs = 0;
		slot->used = false;
		slot->image[0] = '\0';
		slot->mount_point[0] = '\0';
		slot->loop_device[0] = '\0';
		printf("pkgmerp: released %s\n", mount_point);
	}

	pthread_mutex_unlock(&mounts_lock);
	return released;
}
