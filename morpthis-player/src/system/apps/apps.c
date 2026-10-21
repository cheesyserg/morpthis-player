#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/device/system.h"

// How often the parent looks at the child while waiting. Ten milliseconds is
// below the shortest timeout worth having and costs nothing on a core that is
// otherwise asleep.
#define APPS_POLL_MS 10

// A manifest is a few hundred bytes. Four kilobytes is room for a long
// description and a comment or two, and small enough that a file which is not
// a manifest at all is refused before it is parsed.
#define MANIFEST_MAX 4096

static void set_error(apps_result_t *out, const char *text) {
	snprintf(out->error, sizeof(out->error), "%s", text ? text : "unknown error");
}

static void set_errno(apps_result_t *out, const char *what, int err) {
	snprintf(out->error, sizeof(out->error), "%s: %s", what, strerror(err));
}

// A path from a manifest is relative to the app's own folder and may not leave
// it. ".." anywhere is refused rather than resolved: a manifest is data from a
// removable card, and the one thing it must not be able to do is name a file
// outside the folder the player is about to run.
static bool safe_relative(const char *path) {
	if (!path || !path[0] || path[0] == '/') {
		return false;
	}
	if (strstr(path, "..") || strchr(path, '\\')) {
		return false;
	}
	return true;
}

static bool read_manifest(const char *folder, const char *dirname, app_t *app) {
	char path[APPS_FOLDER_MAX + 16];
	int n = snprintf(path, sizeof(path), "%s/app.json", folder);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	char text[MANIFEST_MAX];
	size_t got = fread(text, 1, sizeof(text) - 1, f);
	bool too_big = got == sizeof(text) - 1 && fgetc(f) != EOF;
	fclose(f);
	if (too_big) {
		fprintf(stderr, "apps: %s is larger than %d bytes, ignored\n", path, MANIFEST_MAX);
		return false;
	}
	text[got] = '\0';

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		fprintf(stderr, "apps: %s is not valid JSON, ignored\n", path);
		return false;
	}

	memset(app, 0, sizeof(*app));
	int root = json_root(&doc);
	json_obj_str(&doc, root, "id", app->id, sizeof(app->id));
	json_obj_str(&doc, root, "name", app->name, sizeof(app->name));
	json_obj_str(&doc, root, "version", app->version, sizeof(app->version));
	json_obj_str(&doc, root, "entry", app->entry, sizeof(app->entry));
	json_obj_str(&doc, root, "icon", app->icon, sizeof(app->icon));
	json_obj_str(&doc, root, "description", app->description, sizeof(app->description));
	app->min_player = (int)json_obj_long(&doc, root, "min_player", 0);

	char tier[APPS_TIER_MAX];
	json_obj_str(&doc, root, "tier", tier, sizeof(tier));
	if (!tier[0] || strcasecmp(tier, "light") == 0) {
		app->tier = APP_TIER_LIGHT;
	} else if (strcasecmp(tier, "heavy") == 0) {
		app->tier = APP_TIER_HEAVY;
	} else {
		app->tier = APP_TIER_LIGHT;
		fprintf(stderr, "apps: %s names unknown tier \"%s\", using light\n", path, tier);
	}

	char category[APPS_CATEGORY_MAX];
	json_obj_str(&doc, root, "category", category, sizeof(category));
	if (!category[0] || strcasecmp(category, "app") == 0) {
		app->category = APP_CATEGORY_APP;
	} else if (strcasecmp(category, "game") == 0) {
		app->category = APP_CATEGORY_GAME;
	} else {
		app->category = APP_CATEGORY_APP;
		fprintf(stderr, "apps: %s names unknown category \"%s\", using app\n", path, category);
	}
	json_free(&doc);

	if (!app->id[0]) {
		snprintf(app->id, sizeof(app->id), "%s", dirname);
	}
	if (!app->name[0]) {
		snprintf(app->name, sizeof(app->name), "%s", app->id);
	}
	if (!safe_relative(app->entry)) {
		fprintf(stderr, "apps: %s has no usable entry, ignored\n", path);
		return false;
	}
	if (app->icon[0] && !safe_relative(app->icon)) {
		fprintf(stderr, "apps: %s names an icon outside the app folder, ignored\n", path);
		return false;
	}

	snprintf(app->dir, sizeof(app->dir), "%s", dirname);
	snprintf(app->folder, sizeof(app->folder), "%s", folder);
	return true;
}

static int by_name(const void *a, const void *b) {
	return strcasecmp(((const app_t *)a)->name, ((const app_t *)b)->name);
}

int apps_scan(app_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}

	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		return -1;
	}

	char dir[APPS_FOLDER_MAX];
	int n = snprintf(dir, sizeof(dir), "%s/%s", root, APPS_DIR);
	if (n < 0 || (size_t)n >= sizeof(dir)) {
		return 0;
	}

	DIR *d = opendir(dir);
	if (!d) {
		return 0; // no Apps folder yet is not an error
	}

	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL && count < max) {
		if (entry->d_name[0] == '.') {
			continue;
		}

		char folder[APPS_FOLDER_MAX];
		n = snprintf(folder, sizeof(folder), "%s/%s", dir, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(folder)) {
			continue;
		}

		struct stat st;
		if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}

		if (read_manifest(folder, entry->d_name, &out[count])) {
			count++;
		}
	}
	closedir(d);

	qsort(out, (size_t)count, sizeof(*out), by_name);
	return count;
}

bool apps_icon_path(const app_t *app, char *out, size_t size) {
	if (!app || !app->icon[0] || !out || size == 0) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", app->folder, app->icon);
	return n > 0 && (size_t)n < size;
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

	char path[APPS_FOLDER_MAX + APPS_ENTRY_MAX + 8];
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
	char dir[sizeof(path)];
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

bool apps_launch(const app_t *app, int timeout_ms, apps_result_t *out) {
	if (!out) {
		return false;
	}
	if (!app || !app->entry[0] || !app->dir[0]) {
		memset(out, 0, sizeof(*out));
		set_error(out, "no app");
		return false;
	}

	char relative[APPS_ID_MAX + APPS_ENTRY_MAX + 8];
	int n = snprintf(relative, sizeof(relative), "%s/%s/%s", APPS_DIR, app->dir, app->entry);
	if (n < 0 || (size_t)n >= sizeof(relative)) {
		memset(out, 0, sizeof(*out));
		set_error(out, "path too long");
		return false;
	}
	return apps_run(relative, timeout_ms, out);
}
