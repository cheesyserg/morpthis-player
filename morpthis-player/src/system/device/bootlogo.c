#include "bootlogo.h"

#include "src/system/core/utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// See bootlogo.h for where these numbers come from -- they are the stock
// firmware's own, read out of its init script and its binary, not guesses.
#define MTD_DEVICE "/dev/mtd5"
#define MARKER_OFFSET "0x20000"
// "theme:N logo:M": the stock field, the space the stock script's own read
// never looks past, and the field the block the packer adds reads at byte 8.
#define MARKER_LENGTH 14
#define MARKER_SECOND_FIELD 8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool worker_running;
static volatile int pending_choice; // what the worker should write
static volatile bool pending_dark;

// What this boot did, in two lines: the trace the boot script left in /tmp
// before the player existed, and what the worker made of the flash marker.
// The boot screen is drawn before anything here can log, so the script has to
// leave something behind to be read later -- see the packer's choice block.
static char trace_buf[512];
static bool trace_loaded;
static char status_buf[64];

// The marker for a choice and a theme, as it has to land in the flash. The
// stock pair keeps the theme field the stock script expects; everything else
// asks for theme:3, the value that falls through to /etc/logo.jpeg, and says
// what it really is in the second field.
static void marker_for(int choice, bool dark, char out[MARKER_LENGTH + 1]) {
	int theme = choice == BOOTLOGO_STOCK ? (dark ? 2 : 1) : 3;
	snprintf(out, MARKER_LENGTH + 1, "theme:%d logo:%d", theme, choice);
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

static void *write_main(void *arg) {
	(void)arg;
	thread_be_background("boot logo");

	for (;;) {
		int choice;
		bool dark;
		pthread_mutex_lock(&lock);
		choice = pending_choice;
		dark = pending_dark;
		pthread_mutex_unlock(&lock);

		char want[MARKER_LENGTH + 1];
		marker_for(choice, dark, want);

		// Already right: nothing is written. A theme switched back and forth
		// must not cost an erase cycle each time.
		char have[MARKER_LENGTH + 1];
		if (read_marker(have) && strncmp(have, want, MARKER_LENGTH) == 0) {
			pthread_mutex_lock(&lock);
			snprintf(status_buf, sizeof(status_buf), "marker agrees: %s", want);
			pthread_mutex_unlock(&lock);
			break;
		}

		char cmd[256];
		snprintf(cmd, sizeof(cmd), "flash_erase -q %s %s 1", MTD_DEVICE, MARKER_OFFSET);
		if (system(cmd) != 0) {
			fprintf(stderr, "bootlogo: flash_erase failed\n");
			break;
		}

		// The stock player's own pipeline, %-256s and all, but quoted: the
		// marker carries a space between its two fields now, and an unquoted
		// one is split into words there, which shell printf then prints back
		// to back -- "theme:3logo:2", the second field destroyed. Quoted, the
		// padded marker is one argument and reaches the flash as written;
		// nandwrite's -p fills the rest of the page as before.
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

		// The choice or the theme may have moved again while the flash was
		// busy; the loop looks at the pending pair once more.
		pthread_mutex_lock(&lock);
		bool changed = pending_choice != choice || pending_dark != dark;
		pthread_mutex_unlock(&lock);
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

void bootlogo_set(int choice, bool dark) {
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_TRAVELLING) {
		choice = BOOTLOGO_RETROSPACE;
	}

	if (access(MTD_DEVICE, W_OK) != 0) {
		return; // no such flash here (the host build, or a different device)
	}

	pthread_mutex_lock(&lock);
	pending_choice = choice;
	pending_dark = dark;
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
