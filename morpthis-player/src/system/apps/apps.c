#include "apps.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/device/system.h"

// How often the parent looks at the child while waiting. Ten milliseconds is
// below the shortest timeout worth having and costs nothing on a core that is
// otherwise asleep.
#define APPS_POLL_MS 10

static void set_error(apps_result_t *out, const char *text) {
	snprintf(out->error, sizeof(out->error), "%s", text ? text : "unknown error");
}

static void set_errno(apps_result_t *out, const char *what, int err) {
	snprintf(out->error, sizeof(out->error), "%s: %s", what, strerror(err));
}

// The child inherits the player's handlers -- the crash handler that writes to
// the player's log, the terminate handler, and SIGPIPE ignored. None of them
// belong to an app: a fault in the app should end the app, not be reported as
// a fault in the player. Only async-signal-safe calls are made here, because
// this runs between fork() and exec() in a process with other threads.
static void child_reset_signals(void) {
	signal(SIGPIPE, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
}

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Reaps a child that is already on its way out, so it does not become a zombie
// for the rest of the session.
static void reap(pid_t pid) {
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
}

bool apps_run(const char *relative, int timeout_ms, apps_result_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		set_error(out, "no card");
		return false;
	}
	if (!relative || !relative[0]) {
		set_error(out, "no path");
		return false;
	}

	char path[600];
	if (snprintf(path, sizeof(path), "%s/%s", root, relative) >= (int)sizeof(path)) {
		set_error(out, "path too long");
		return false;
	}

	struct stat st;
	if (stat(path, &st) != 0) {
		set_errno(out, path, errno);
		return false;
	}
	if (!S_ISREG(st.st_mode)) {
		set_error(out, "not a file");
		return false;
	}
	if (access(path, X_OK) != 0) {
		set_errno(out, "not executable", errno);
		return false;
	}

	// The app's own folder, so a binary can find the files beside it without
	// knowing where the card is mounted.
	char dir[600];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
	}

	// The pipe is how the parent tells "execve() failed" from "the app exited
	// with 127": the write end is close-on-exec, so a successful exec leaves
	// the parent reading EOF, and a failed one leaves an errno behind.
	int errpipe[2];
	if (pipe(errpipe) != 0) {
		set_errno(out, "pipe", errno);
		return false;
	}
	if (fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) < 0) {
		set_errno(out, "fcntl", errno);
		close(errpipe[0]);
		close(errpipe[1]);
		return false;
	}

	long started_at = now_ms();
	fprintf(stderr, "apps: running %s\n", path);

	pid_t pid = fork();
	if (pid < 0) {
		set_errno(out, "fork", errno);
		close(errpipe[0]);
		close(errpipe[1]);
		return false;
	}

	if (pid == 0) {
		close(errpipe[0]);
		child_reset_signals();

		if (chdir(dir) != 0) {
			int err = errno;
			ssize_t ignored = write(errpipe[1], &err, sizeof(err));
			(void)ignored;
			_exit(126);
		}

		char *argv[] = {path, NULL};
		execv(path, argv);

		int err = errno;
		ssize_t ignored = write(errpipe[1], &err, sizeof(err));
		(void)ignored;
		_exit(127);
	}

	close(errpipe[1]);

	// Wait for the exec handshake, bounded by the same timeout as the app
	// itself: a child that never reaches execve() -- a chdir on a card that
	// has just died, say -- must not hold this thread forever.
	int exec_errno = 0;
	struct pollfd pfd = {.fd = errpipe[0], .events = POLLIN};
	int ready;
	do {
		ready = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
	} while (ready < 0 && errno == EINTR);

	if (ready == 0) {
		close(errpipe[0]);
		kill(pid, SIGKILL);
		reap(pid);
		out->timed_out = true;
		out->signal = SIGKILL;
		set_error(out, "timed out before exec");
		fprintf(stderr, "apps: %s did not reach exec within %d ms\n", path, timeout_ms);
		return false;
	}

	ssize_t got;
	do {
		got = read(errpipe[0], &exec_errno, sizeof(exec_errno));
	} while (got < 0 && errno == EINTR);
	close(errpipe[0]);

	if (got == (ssize_t)sizeof(exec_errno)) {
		reap(pid);
		set_errno(out, "exec", exec_errno);
		fprintf(stderr, "apps: %s could not be started: %s\n", path, out->error);
		return false;
	}

	out->started = true;

	long deadline = started_at + (timeout_ms > 0 ? timeout_ms : 0);
	for (;;) {
		int status = 0;
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid) {
			if (WIFEXITED(status)) {
				out->exit_code = WEXITSTATUS(status);
				fprintf(stderr, "apps: %s exited with %d\n", path, out->exit_code);
			} else if (WIFSIGNALED(status)) {
				out->signal = WTERMSIG(status);
				fprintf(stderr, "apps: %s killed by signal %d\n", path, out->signal);
			}
			return true;
		}
		if (done < 0 && errno != EINTR) {
			set_errno(out, "waitpid", errno);
			return true;
		}

		if (timeout_ms > 0 && now_ms() >= deadline) {
			fprintf(stderr, "apps: %s outlived %d ms, killing it\n", path, timeout_ms);
			kill(pid, SIGKILL);
			reap(pid);
			out->timed_out = true;
			out->signal = SIGKILL;
			return true;
		}

		usleep(APPS_POLL_MS * 1000);
	}
}
