#include "bootlogo.h"

#include "src/system/core/utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// See bootlogo.h for where these numbers come from -- they are the stock
// firmware's own, read out of its init script and its binary, not guesses.
#define MTD_DEVICE "/dev/mtd5"
#define MARKER_OFFSET "0x20000"
// The marker's own page, and the page after the padding where a custom
// picture starts. Two pages in rather than one: nandwrite -p pads the marker
// to the NAND's page size, which is 8 KiB on some of these parts, and a
// picture one page in would have its first half overwritten by the marker.
#define IMAGE_OFFSET "0x22000"
// "theme:N logo:M size:S hash:H", padded with spaces to one fixed width. The
// stock script reads the first seven bytes and never looks further; the
// packer's block reads all of it.
#define MARKER_LENGTH 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool worker_running;
static volatile int pending_choice; // what the worker should write
static volatile bool pending_dark;
static volatile bool pending_force; // write even when the marker agrees

// What this boot did, in two lines: the trace the boot script left in /tmp
// before the player existed, and what the worker made of the flash marker.
// The boot screen is drawn before anything here can log, so the script has to
// leave something behind to be read later -- see the packer's choice block.
static char trace_buf[512];
static bool trace_loaded;
static char status_buf[128];

// The marker for a choice and a theme, as it has to land in the flash. The
// stock pair keeps the theme field the stock script expects; everything else
// asks for theme:3, the value that falls through to /etc/logo.jpeg, and says
// what it really is in the second field. A custom picture adds its length and
// a hash of its bytes, so the worker can tell whether the block already holds
// the picture it is about to write.
static void marker_for(int choice, bool dark, size_t image_size, uint32_t image_hash, char out[MARKER_LENGTH + 1]) {
	int theme = choice == BOOTLOGO_STOCK ? (dark ? 2 : 1) : 3;
	int logo = choice == BOOTLOGO_CUSTOM ? 2 : choice;

	if (choice == BOOTLOGO_CUSTOM) {
		snprintf(out, MARKER_LENGTH + 1, "theme:%d logo:%d size:%u hash:%08x", theme, logo, (unsigned)image_size,
				 image_hash);
	} else {
		snprintf(out, MARKER_LENGTH + 1, "theme:%d logo:%d", theme, logo);
	}

	// The stock player pads its marker with spaces to 256 bytes; doing the
	// same here means a read of MARKER_LENGTH bytes compares like with like.
	size_t used = strlen(out);
	while (used < MARKER_LENGTH) {
		out[used++] = ' ';
	}
	out[MARKER_LENGTH] = '\0';
}

// The marker as it is now, or false when there is nothing to read. The stock
// player reads it the same way, into a temp file: nanddump wants somewhere to
// put its bytes and will not simply hand them back.
static bool read_marker(char out[MARKER_LENGTH + 1]) {
	if (access(MTD_DEVICE, R_OK) != 0) {
		return false;
	}

	char cmd[256];
	const char *tmp = "/tmp/.bootlogo_marker";
	snprintf(cmd, sizeof(cmd), "nanddump -q -s %s -l %d %s -a > %s 2>/dev/null", MARKER_OFFSET, MARKER_LENGTH,
			 MTD_DEVICE, tmp);
	if (system(cmd) != 0) {
		return false;
	}

	FILE *f = fopen(tmp, "rb");
	if (!f) {
		return false;
	}
	size_t got = fread(out, 1, MARKER_LENGTH, f);
	fclose(f);
	remove(tmp);

	if (got < MARKER_LENGTH) {
		return false;
	}
	out[MARKER_LENGTH] = '\0';
	return true;
}

// The applied custom picture's length and FNV-1a hash, or false when it is
// missing or too large for the block. Read in one pass: the file is at most
// BOOTLOGO_IMAGE_MAX and this runs on the worker.
static bool custom_image_info(size_t *size, uint32_t *hash) {
	FILE *f = fopen(BOOTLOGO_CUSTOM_IMAGE, "rb");
	if (!f) {
		return false;
	}

	uint32_t h = 2166136261u;
	size_t total = 0;
	uint8_t buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		for (size_t i = 0; i < got; i++) {
			h ^= buf[i];
			h *= 16777619u;
		}
		total += got;
		if (total > BOOTLOGO_IMAGE_MAX) {
			break;
		}
	}
	fclose(f);

	if (total == 0 || total > BOOTLOGO_IMAGE_MAX) {
		return false;
	}

	*size = total;
	*hash = h;
	return true;
}

static void *write_main(void *arg) {
	(void)arg;
	thread_be_background("boot logo");

	for (;;) {
		int choice;
		bool dark;
		bool force;
		pthread_mutex_lock(&lock);
		choice = pending_choice;
		dark = pending_dark;
		force = pending_force;
		pending_force = false;
		pthread_mutex_unlock(&lock);

		int requested_choice = choice;
		size_t image_size = 0;
		uint32_t image_hash = 0;
		if (choice == BOOTLOGO_CUSTOM && !custom_image_info(&image_size, &image_hash)) {
			// The picture was removed or is too large: the marker must not
			// point at something the boot script cannot draw.
			choice = BOOTLOGO_RETROSPACE;
		}

		char want[MARKER_LENGTH + 1];
		marker_for(choice, dark, image_size, image_hash, want);

		// Already right: nothing is written. A theme switched back and forth
		// must not cost an erase cycle each time, and neither must a theme
		// switch while a custom picture is applied. A forced write skips this:
		// the marker can agree while the picture behind it is damaged.
		char have[MARKER_LENGTH + 1];
		if (!force && read_marker(have) && strncmp(have, want, MARKER_LENGTH) == 0) {
			pthread_mutex_lock(&lock);
			snprintf(status_buf, sizeof(status_buf), "marker agrees: %s", want);
			pthread_mutex_unlock(&lock);
			break;
		}

		char cmd[512];
		snprintf(cmd, sizeof(cmd), "flash_erase -q %s %s 1", MTD_DEVICE, MARKER_OFFSET);
		if (system(cmd) != 0) {
			fprintf(stderr, "bootlogo: flash_erase failed\n");
			break;
		}

		// The picture first, the marker second: the marker is what points at
		// it, so a failed picture write leaves the boot script falling back to
		// the stock chain rather than drawing half a file.
		if (choice == BOOTLOGO_CUSTOM) {
			snprintf(cmd, sizeof(cmd), "nandwrite -q -s %s -p %s < %s", IMAGE_OFFSET, MTD_DEVICE,
					 BOOTLOGO_CUSTOM_IMAGE);
			if (system(cmd) != 0) {
				fprintf(stderr, "bootlogo: nandwrite of the custom picture failed\n");
				break;
			}
		}

		// The stock player's own pipeline, %-256s and all, but quoted: the
		// marker carries spaces between its fields now, and an unquoted one is
		// split into words there, which shell printf then prints back to back.
		// Quoted, the padded marker is one argument and reaches the flash as
		// written; nandwrite's -p fills the rest of the page as before.
		char write_cmd[512];
		snprintf(write_cmd, sizeof(write_cmd), "printf '%%-256s' '%s' | nandwrite -q -s %s -p %s -", want,
				 MARKER_OFFSET, MTD_DEVICE);
		if (system(write_cmd) != 0) {
			fprintf(stderr, "bootlogo: nandwrite failed\n");
			break;
		}

		printf("bootlogo: marker \"%s\" written to %s\n", want, MTD_DEVICE);
		pthread_mutex_lock(&lock);
		snprintf(status_buf, sizeof(status_buf), "marker written: %s", want);
		pthread_mutex_unlock(&lock);

		// The choice, the theme or the picture may have moved again while the
		// flash was busy; the loop looks at the pending pair and at the file
		// once more. The file matters because applying a second picture keeps
		// the same choice: only its bytes say the block is out of date.
		pthread_mutex_lock(&lock);
		bool changed = pending_choice != requested_choice || pending_dark != dark || pending_force;
		pthread_mutex_unlock(&lock);

		if (!changed && requested_choice == BOOTLOGO_CUSTOM) {
			size_t now_size = 0;
			uint32_t now_hash = 0;
			if (custom_image_info(&now_size, &now_hash)) {
				changed = choice != BOOTLOGO_CUSTOM || now_size != image_size || now_hash != image_hash;
			}
		}

		if (!changed) {
			break;
		}
	}

	pthread_mutex_lock(&lock);
	worker_running = false;
	pthread_mutex_unlock(&lock);
	return NULL;
}

const char *bootlogo_trace(void) {
	pthread_mutex_lock(&lock);
	if (!trace_loaded) {
		trace_loaded = true;
		FILE *f = fopen("/tmp/.bootlogo_trace", "rb");
		if (f) {
			size_t got = fread(trace_buf, 1, sizeof(trace_buf) - 1, f);
			trace_buf[got] = '\0';
			fclose(f);
		}
	}
	const char *out = trace_buf[0] ? trace_buf : NULL;
	pthread_mutex_unlock(&lock);
	return out;
}

const char *bootlogo_status(void) {
	pthread_mutex_lock(&lock);
	const char *out = status_buf[0] ? status_buf : NULL;
	pthread_mutex_unlock(&lock);
	return out;
}

bool bootlogo_custom_available(void) {
	struct stat st;
	if (stat(BOOTLOGO_CUSTOM_IMAGE, &st) != 0) {
		return false;
	}
	return st.st_size > 0 && (size_t)st.st_size <= BOOTLOGO_IMAGE_MAX;
}

const char *bootlogo_custom_path(void) { return BOOTLOGO_CUSTOM_IMAGE; }

bool bootlogo_custom_store(const uint8_t *jpeg, size_t len) {
	if (!jpeg || len == 0 || len > BOOTLOGO_IMAGE_MAX) {
		return false;
	}

	const char *tmp = BOOTLOGO_CUSTOM_IMAGE ".tmp";
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}

	bool ok = fwrite(jpeg, 1, len, f) == len;
	if (ok) {
		ok = fflush(f) == 0;
	}
	if (ok) {
		ok = fsync(fileno(f)) == 0;
	}
	fclose(f);

	if (!ok) {
		remove(tmp);
		return false;
	}
	if (rename(tmp, BOOTLOGO_CUSTOM_IMAGE) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static void bootlogo_set_internal(int choice, bool dark, bool force) {
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_CUSTOM) {
		choice = BOOTLOGO_RETROSPACE;
	}

	if (access(MTD_DEVICE, W_OK) != 0) {
		return; // no such flash here (the host build, or a different device)
	}

	pthread_mutex_lock(&lock);
	pending_choice = choice;
	pending_dark = dark;
	if (force) {
		pending_force = true;
	}
	if (worker_running) {
		pthread_mutex_unlock(&lock); // the one already running will pick it up
		return;
	}
	worker_running = true;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, write_main, NULL) != 0) {
		pthread_mutex_lock(&lock);
		worker_running = false;
		pthread_mutex_unlock(&lock);
		return;
	}
	pthread_detach(thread);
}

void bootlogo_set(int choice, bool dark) { bootlogo_set_internal(choice, dark, false); }

void bootlogo_set_force(int choice, bool dark) { bootlogo_set_internal(choice, dark, true); }
