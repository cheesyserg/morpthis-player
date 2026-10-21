#ifndef APPS_H
#define APPS_H

#include <stdbool.h>

// Running an executable from the card.
//
// This is the first step of the sideloading work: a developer tool that proves
// the player can start a binary that lives on the microSD card, and reports
// what happened. It is deliberately small -- no manifest, no sandbox, no app
// page -- so that the one thing it tests is the thing that cannot be tested
// from a desktop: execve() on the card's exfat mount, from this process, with
// this process's environment.
//
// The card is mounted by the player (or found already mounted) with no
// MS_NOEXEC, and exfat's fmask=0022 leaves files at 0755, so the execute bit is
// there. What is not obvious until it is tried is whether the kernel's exfat
// driver lets an executable be mapped from the card at all, and whether the
// player's own threads and signal handlers get in the way. That is what this
// answers.

typedef struct {
	// False when the child never got as far as execve(): no card, no file, no
	// execute permission, or execve() itself refused. `error` says which.
	bool started;

	// True when the child was killed for outliving the timeout. `signal` is
	// then SIGKILL.
	bool timed_out;

	// The child's exit status, when it exited on its own.
	int exit_code;

	// The signal that ended it, or 0 when it exited.
	int signal;

	// Why it could not be started, when `started` is false. Never empty then.
	char error[128];
} apps_result_t;

// Runs `relative` (e.g. "Apps/hello") under the card root, with the app's own
// folder as its working directory, and waits for it.
//
// Blocks the calling thread, so call it from a worker, not the GUI thread.
// The child runs with the player's uid and environment: this is a developer
// tool, not a security boundary.
//
// Returns out->started.
bool apps_run(const char *relative, int timeout_ms, apps_result_t *out);

#endif /* APPS_H */
