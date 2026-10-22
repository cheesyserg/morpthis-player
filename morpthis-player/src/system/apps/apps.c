#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/apps/pkgmerp.h"
#include "src/system/core/json.h"
#include "src/system/device/system.h"

extern char **environ;

// The tool child apps_run() is waiting for, or 0. A tool runs in its own
// session (setsid below), so the GUI can ask the whole tree to stop before the
// card is handed to a PC. Atomic because the launcher and the worker are on
// different threads.
static volatile pid_t running_tool_pid;

static void set_running_tool(pid_t pid) {
	__atomic_store_n(&running_tool_pid, pid, __ATOMIC_SEQ_CST);
}

// The cache for icons read out of a package. The image is mounted only while
// the page scans; the tile needs the picture afterwards, so the few kilobytes
// of an icon are copied out here. /tmp is the right home for it: it is small,
// it is rebuilt on every scan, and a reboot cleans it up.
#define APPS_ICON_CACHE "/tmp/morpthis/pkgmerp-icons"

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

static void set_text(char *out, size_t size, const char *format, ...) {
	if (!out || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(out, size, format, args);
	va_end(args);
}

static void set_errno(apps_result_t *out, const char *what, int err) {
	snprintf(out->error, sizeof(out->error), "%s: %s", what, strerror(err));
}

// A path from a manifest is relative to the package root and may not leave
// it. ".." anywhere is refused rather than resolved: a manifest is data from a
// removable card, and the one thing it must not be able to do is name a file
// outside the image the player is about to run.
static bool safe_relative(const char *path) {
	if (!path || !path[0] || path[0] == '/') {
		return false;
	}
	if (strstr(path, "..") || strchr(path, '\\')) {
		return false;
	}
	return true;
}

// mkdir -p. Used for the icon cache and the data folder, both of which sit
// under a parent the player may not have made yet.
static int make_directories(const char *path) {
	char work[APPS_FOLDER_MAX];
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

// The data folder used to be <card>/merp_data. Move it under Apps, beside
// pkgmerps, so the card has one app tree. The whole folder is renamed in one
// step when the destination is free; if a previous partial move left both, each
// app folder is moved into the new root and an existing one is kept as it is.
// A failure here is not fatal: the app falls back to a fresh data folder.
static void migrate_legacy_data(const char *root) {
	if (!root || !root[0]) {
		return;
	}

	char legacy[APPS_FOLDER_MAX];
	char current[APPS_FOLDER_MAX];
	int n = snprintf(legacy, sizeof(legacy), "%s/%s", root, APPS_LEGACY_DATA_DIR);
	if (n < 0 || (size_t)n >= sizeof(legacy)) {
		return;
	}
	n = snprintf(current, sizeof(current), "%s/%s", root, APPS_DATA_DIR);
	if (n < 0 || (size_t)n >= sizeof(current)) {
		return;
	}

	struct stat st;
	if (stat(legacy, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}

	if (rename(legacy, current) == 0) {
		fprintf(stderr, "apps: moved %s to %s\n", legacy, current);
		return;
	}

	// The destination exists already (or the rename was refused). Create it and
	// move the app folders one by one, never over a folder that is already there.
	if (make_directories(current) != 0 && errno != EEXIST) {
		return;
	}

	DIR *d = opendir(legacy);
	if (!d) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		char from[APPS_FOLDER_MAX];
		char to[APPS_FOLDER_MAX];
		n = snprintf(from, sizeof(from), "%s/%s", legacy, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(from)) {
			continue;
		}
		n = snprintf(to, sizeof(to), "%s/%s", current, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(to)) {
			continue;
		}
		if (stat(to, &st) == 0) {
			continue; // both copies: keep the one already migrated
		}
		if (rename(from, to) == 0) {
			fprintf(stderr, "apps: moved %s to %s\n", from, to);
		}
	}
	closedir(d);
	rmdir(legacy); // only succeeds when every entry moved
}

static bool copy_file(const char *from, const char *to) {
	char buffer[4096];
	FILE *in = fopen(from, "rb");
	if (!in) {
		return false;
	}
	FILE *out_file = fopen(to, "wb");
	if (!out_file) {
		fclose(in);
		return false;
	}

	bool ok = true;
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (fwrite(buffer, 1, got, out_file) != got) {
			ok = false;
			break;
		}
	}
	if (ferror(in)) {
		ok = false;
	}
	if (fclose(out_file) != 0) {
		ok = false;
	}
	fclose(in);

	if (!ok) {
		remove(to);
	}
	return ok;
}

// Copies the app's icon out of the mounted package. After the unmount the
// image is gone from the namespace, and the tile still has to draw.
static void cache_icon(const char *package_root, app_t *app) {
	if (!app->icon[0]) {
		return;
	}

	char source[APPS_FOLDER_MAX + APPS_ICON_MAX + 2];
	int n = snprintf(source, sizeof(source), "%s/%s", package_root, app->icon);
	if (n < 0 || (size_t)n >= sizeof(source)) {
		app->icon[0] = '\0';
		return;
	}

	if (make_directories(APPS_ICON_CACHE) != 0) {
		fprintf(stderr, "apps: %s: icon cache %s: %s\n", app->id, APPS_ICON_CACHE, strerror(errno));
		app->icon[0] = '\0';
		return;
	}

	char cached[APPS_ICON_MAX + 64];
	n = snprintf(cached, sizeof(cached), "%s/%s.img", APPS_ICON_CACHE, app->dir);
	if (n < 0 || (size_t)n >= sizeof(cached) || !copy_file(source, cached)) {
		fprintf(stderr, "apps: %s: icon %s could not be cached\n", app->id, source);
		app->icon[0] = '\0';
		return;
	}

	snprintf(app->icon, sizeof(app->icon), "%s", cached);
	app->icon_cached = true;
}

static bool package_name(const char *name) {
	size_t length = strlen(name);
	size_t ext = strlen(APPS_PACKAGE_EXT);
	return length > ext && strcasecmp(name + length - ext, APPS_PACKAGE_EXT) == 0;
}

static void package_id(const char *name, char *out, size_t size) {
	size_t length = strlen(name) - strlen(APPS_PACKAGE_EXT);
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, name, length);
	out[length] = '\0';
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
		fprintf(stderr, "apps: %s names an icon outside the package, ignored\n", path);
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

	migrate_legacy_data(root);

	char dir[APPS_FOLDER_MAX];
	int n = snprintf(dir, sizeof(dir), "%s/%s/%s", root, APPS_DIR, APPS_PACKAGE_DIR);
	if (n < 0 || (size_t)n >= sizeof(dir)) {
		return 0;
	}

	DIR *d = opendir(dir);
	if (!d) {
		return 0; // no pkgmerps folder yet is not an error
	}

	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL && count < max) {
		if (entry->d_name[0] == '.' || !package_name(entry->d_name)) {
			continue;
		}

		char image[APPS_FOLDER_MAX];
		n = snprintf(image, sizeof(image), "%s/%s", dir, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(image)) {
			continue;
		}

		struct stat st;
		if (stat(image, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		char id[APPS_ID_MAX];
		package_id(entry->d_name, id, sizeof(id));

		char mount_point[APPS_FOLDER_MAX];
		if (!pkgmerp_mount_point(id, mount_point, sizeof(mount_point))) {
			continue;
		}

		// A running app's package may already be mounted; the registry hands
		// out another reference and the unmount below drops only this scan's.
		char error[128];
		if (!pkgmerp_mount(image, mount_point, error, sizeof(error))) {
			fprintf(stderr, "apps: %s: %s, ignored\n", image, error);
			continue;
		}

		app_t app;
		bool readable = read_manifest(mount_point, id, &app);
		if (readable) {
			snprintf(app.package, sizeof(app.package), "%s", image);
			n = snprintf(app.data, sizeof(app.data), "%s/%s/%s", root, APPS_DATA_DIR, id);
			if (n < 0 || (size_t)n >= sizeof(app.data)) {
				app.data[0] = '\0';
			}
			cache_icon(mount_point, &app);
		}

		pkgmerp_unmount(mount_point, error, sizeof(error));
		if (readable) {
			out[count++] = app;
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
	int n;
	if (app->icon_cached) {
		n = snprintf(out, size, "%s", app->icon);
	} else {
		n = snprintf(out, size, "%s/%s", app->folder, app->icon);
	}
	return n > 0 && (size_t)n < size;
}

bool apps_mount(app_t *app, char *error, size_t error_size) {
	if (!app || !app->package[0]) {
		set_text(error, error_size, "no package");
		return false;
	}

	if (!app->folder[0] &&
		!pkgmerp_mount_point(app->dir, app->folder, sizeof(app->folder))) {
		set_text(error, error_size, "no mount point");
		return false;
	}

	char why[128];
	if (!pkgmerp_mount(app->package, app->folder, why, sizeof(why))) {
		set_text(error, error_size, "%s", why);
		return false;
	}

	const char *root = storage_sd_root();
	if (root && root[0]) {
		migrate_legacy_data(root);
	}
	if (!app->data[0] && root && root[0]) {
		int n = snprintf(app->data, sizeof(app->data), "%s/%s/%s", root, APPS_DATA_DIR, app->dir);
		if (n < 0 || (size_t)n >= sizeof(app->data)) {
			app->data[0] = '\0';
		}
	}

	// The package is read-only, so this is where everything the app saves
	// lives. A card that cannot take it still lets the app run; its writes
	// fail through the SDK, which is the honest outcome.
	if (app->data[0] && make_directories(app->data) != 0 && errno != EEXIST) {
		fprintf(stderr, "apps: %s: data folder %s: %s\n", app->id, app->data, strerror(errno));
	}

	return true;
}

void apps_unmount(const char *mount_point) {
	if (!mount_point || !mount_point[0]) {
		return;
	}
	char error[128];
	if (!pkgmerp_unmount(mount_point, error, sizeof(error))) {
		fprintf(stderr, "apps: could not release %s: %s\n", mount_point, error);
	}
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

void apps_stop_running_tool(void) {
	pid_t pid = __atomic_load_n(&running_tool_pid, __ATOMIC_SEQ_CST);
	if (pid <= 0) {
		return;
	}

	// The child is its own session and process group. Signal the group for
	// anything it started, then the child itself in case setsid() failed.
	fprintf(stderr, "apps: asking tool %d to stop\n", (int)pid);
	kill(-pid, SIGTERM);
	kill(pid, SIGTERM);
}

#define APPS_ENV_FOLDER "MORPTHIS_APP_FOLDER="
#define APPS_ENV_DATA "MORPTHIS_APP_DATA="

// Builds the child's environment before fork(). setenv() after fork() is not
// async-signal-safe in a process with threads, so the two variables are made
// here and handed to execve() instead.
static char **build_child_env(const char *folder, const char *data,
							  char **folder_var, char **data_var) {
	*folder_var = NULL;
	*data_var = NULL;

	size_t count = 0;
	while (environ && environ[count]) {
		count++;
	}

	char **envp = calloc(count + 3, sizeof(char *));
	if (!envp) {
		return NULL;
	}

	size_t used = 0;
	for (size_t i = 0; i < count; i++) {
		if (strncmp(environ[i], APPS_ENV_FOLDER, strlen(APPS_ENV_FOLDER)) == 0 ||
			strncmp(environ[i], APPS_ENV_DATA, strlen(APPS_ENV_DATA)) == 0) {
			continue;
		}
		envp[used++] = environ[i];
	}

	if (folder && folder[0]) {
		size_t n = strlen(APPS_ENV_FOLDER) + strlen(folder) + 1;
		*folder_var = malloc(n);
		if (!*folder_var) {
			free(envp);
			return NULL;
		}
		snprintf(*folder_var, n, "%s%s", APPS_ENV_FOLDER, folder);
		envp[used++] = *folder_var;
	}

	if (data && data[0]) {
		size_t n = strlen(APPS_ENV_DATA) + strlen(data) + 1;
		*data_var = malloc(n);
		if (!*data_var) {
			free(*folder_var);
			*folder_var = NULL;
			free(envp);
			return NULL;
		}
		snprintf(*data_var, n, "%s%s", APPS_ENV_DATA, data);
		envp[used++] = *data_var;
	}

	envp[used] = NULL;
	return envp;
}

bool apps_run(const char *path, const char *folder, const char *data,
			  int timeout_ms, apps_result_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (!path || !path[0] || path[0] != '/') {
		set_error(out, "no path");
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

	// The package root, so a binary can find the files beside it without
	// knowing where it was mounted.
	char derived[APPS_FOLDER_MAX + APPS_ENTRY_MAX + 8];
	const char *working = folder;
	if (!working || !working[0]) {
		snprintf(derived, sizeof(derived), "%s", path);
		char *slash = strrchr(derived, '/');
		if (slash == derived) {
			derived[1] = '\0';
		} else if (slash) {
			*slash = '\0';
		}
		working = derived;
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

	char *folder_var = NULL;
	char *data_var = NULL;
	char **envp = build_child_env(working, data, &folder_var, &data_var);

	long started_at = now_ms();
	fprintf(stderr, "apps: running %s\n", path);

	pid_t pid = fork();
	if (pid < 0) {
		set_errno(out, "fork", errno);
		close(errpipe[0]);
		close(errpipe[1]);
		free(folder_var);
		free(data_var);
		free(envp);
		return false;
	}

	if (pid == 0) {
		close(errpipe[0]);
		child_reset_signals();

		// A session of its own, so stopping a tool stops anything it started
		// and cannot reach the player's process group.
		setsid();

		if (chdir(working) != 0) {
			int err = errno;
			ssize_t ignored = write(errpipe[1], &err, sizeof(err));
			(void)ignored;
			_exit(126);
		}

		char *argv[] = {(char *)path, NULL};
		if (envp) {
			execve(path, argv, envp);
		} else {
			execv(path, argv);
		}

		int err = errno;
		ssize_t ignored = write(errpipe[1], &err, sizeof(err));
		(void)ignored;
		_exit(127);
	}

	// The child has its own copy of the environment now.
	free(folder_var);
	free(data_var);
	free(envp);
	close(errpipe[1]);
	set_running_tool(pid);

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
		set_running_tool(0);
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
		set_running_tool(0);
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
			set_running_tool(0);
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
			set_running_tool(0);
			set_errno(out, "waitpid", errno);
			return true;
		}

		if (timeout_ms > 0 && now_ms() >= deadline) {
			fprintf(stderr, "apps: %s outlived %d ms, killing it\n", path, timeout_ms);
			kill(pid, SIGKILL);
			reap(pid);
			set_running_tool(0);
			out->timed_out = true;
			out->signal = SIGKILL;
			return true;
		}

		usleep(APPS_POLL_MS * 1000);
	}
}

bool apps_launch(app_t *app, int timeout_ms, apps_result_t *out) {
	if (!out) {
		return false;
	}
	if (!app || !app->entry[0] || !app->dir[0]) {
		memset(out, 0, sizeof(*out));
		set_error(out, "no app");
		return false;
	}

	char error[128];
	if (!apps_mount(app, error, sizeof(error))) {
		memset(out, 0, sizeof(*out));
		set_error(out, error);
		return false;
	}

	char path[APPS_FOLDER_MAX + APPS_ENTRY_MAX + 8];
	int n = snprintf(path, sizeof(path), "%s/%s", app->folder, app->entry);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		apps_unmount(app->folder);
		memset(out, 0, sizeof(*out));
		set_error(out, "path too long");
		return false;
	}

	bool started = apps_run(path, app->folder, app->data, timeout_ms, out);
	apps_unmount(app->folder);
	return started;
}
