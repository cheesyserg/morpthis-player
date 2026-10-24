#include "sdk_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/apps/apphost.h"
#include "src/gui/shell/settingsrow.h"

// ---------------------------------------------------------------------------
// The app context and posted callbacks
// ---------------------------------------------------------------------------

typedef struct sdk_post {
	const morpthis_app_t *app;
	morpthis_post_fn fn;
	void *user;
	struct sdk_post *next;
} sdk_post_t;

static pthread_mutex_t sdk_lock = PTHREAD_MUTEX_INITIALIZER;
static sdk_post_t *sdk_posts;
static char sdk_log_id[APPS_ID_MAX] = "app";

void sdk_set_app(morpthis_app_t *app) {
	pthread_mutex_lock(&sdk_lock);
	sdk_app = app;
	if (app) {
		snprintf(sdk_log_id, sizeof(sdk_log_id), "%s", app->id);
	} else {
		snprintf(sdk_log_id, sizeof(sdk_log_id), "app");
	}
	pthread_mutex_unlock(&sdk_lock);
}

// Called by the host immediately before the context is freed. Anything still
// waiting in the GUI queue is detached, so its trampoline cannot enter a shared
// object that is about to be dlclose()d.
void sdk_detach_app(morpthis_app_t *app) {
	pthread_mutex_lock(&sdk_lock);
	if (sdk_app == app) {
		sdk_app = NULL;
	}
	for (sdk_post_t *post = sdk_posts; post; post = post->next) {
		if (post->app == app) {
			post->app = NULL;
		}
	}
	pthread_mutex_unlock(&sdk_lock);
}

static void post_run_cb(void *user) {
	sdk_post_t *post = user;

	pthread_mutex_lock(&sdk_lock);
	sdk_post_t **link = &sdk_posts;
	while (*link && *link != post) {
		link = &(*link)->next;
	}
	if (*link == post) {
		*link = post->next;
	}
	bool deliver = post->app && post->app->alive;
	pthread_mutex_unlock(&sdk_lock);

	if (deliver) {
		post->fn(post->user);
	}
	free(post);
}

void morpthis_app_post(morpthis_post_fn fn, void *user) {
	if (!fn) {
		return;
	}

	pthread_mutex_lock(&sdk_lock);
	if (!sdk_app || !sdk_app->alive) {
		pthread_mutex_unlock(&sdk_lock);
		return;
	}
	sdk_post_t *post = calloc(1, sizeof(*post));
	if (!post) {
		pthread_mutex_unlock(&sdk_lock);
		return;
	}
	post->app = sdk_app;
	post->fn = fn;
	post->user = user;
	post->next = sdk_posts;
	sdk_posts = post;
	pthread_mutex_unlock(&sdk_lock);

	if (!gui_post(post_run_cb, post)) {
		pthread_mutex_lock(&sdk_lock);
		sdk_post_t **link = &sdk_posts;
		while (*link && *link != post) {
			link = &(*link)->next;
		}
		if (*link == post) {
			*link = post->next;
		}
		pthread_mutex_unlock(&sdk_lock);
		free(post);
	}
}

void sdk_dispatch_button(morpthis_button_t button, morpthis_input_action_t action) {
	morpthis_app_t *app = sdk_app;
	if (app && app->alive && app->button_cb) {
		app->button_cb(button, action, app->button_user);
	}
}

// ---------------------------------------------------------------------------
// Helpers shared with the UI and filesystem implementations
// ---------------------------------------------------------------------------

char *sdk_strdup(const char *text) {
	if (!text) {
		text = "";
	}
	size_t size = strlen(text) + 1;
	char *copy = malloc(size);
	if (copy) {
		memcpy(copy, text, size);
	}
	return copy;
}

bool sdk_safe_relative(const char *path) {
	if (!path || !path[0] || path[0] == '/') {
		return false;
	}
	return !strstr(path, "..") && !strchr(path, '\\');
}

int sdk_resolve_in(const char *base, const char *relative, char *out, size_t size) {
	if (!base || !base[0]) {
		errno = ENODEV;
		return -1;
	}
	if (!sdk_safe_relative(relative)) {
		errno = EINVAL;
		return -1;
	}
	int n = snprintf(out, size, "%s/%s", base, relative);
	if (n < 0 || (size_t)n >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int sdk_resolve_path(const char *relative, char *out, size_t size) {
	return sdk_resolve_in(sdk_app ? sdk_app->folder : NULL, relative, out, size);
}

int sdk_content_top(void) {
	return sdk_cfg ? settingsrow_content_top(sdk_cfg) : 0;
}

size_t sdk_mem_available(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return SIZE_MAX;
	}
	char line[128];
	unsigned long long kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return kb ? (size_t)kb * 1024u : SIZE_MAX;
}

// ---------------------------------------------------------------------------
// app.h
// ---------------------------------------------------------------------------

const char *morpthis_app_id(const morpthis_app_t *app) {
	return app ? app->id : "";
}

const char *morpthis_app_name(const morpthis_app_t *app) {
	return app ? app->name : "";
}

const char *morpthis_app_version(const morpthis_app_t *app) {
	return app ? app->version : "";
}

const char *morpthis_app_folder(const morpthis_app_t *app) {
	return app ? app->folder : "";
}

int morpthis_app_api_version(const morpthis_app_t *app) {
	(void)app;
	return MORPTHIS_APP_API_VERSION;
}

size_t morpthis_app_mem_available(void) {
	return sdk_mem_available();
}

uint32_t morpthis_app_time_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void morpthis_app_quit(void) {
	apphost_request_quit();
}

bool morpthis_app_background(void) {
	// Player-owned chrome must not outlive the app on screen: a popover, dialog,
	// prompt or screen hold belongs to the page that is being put away.
	sdk_ui_app_reset(false);
	// Only the SCREEN hold is released here. AWAKE and NETWORK describe what the
	// app is still doing while it is away -- a station playing, a download in
	// flight -- and power.h promises that holds survive backgrounding.
	sdk_system_app_background();
	return apphost_background();
}

bool morpthis_app_is_background(void) {
	return sdk_app && sdk_app->alive && sdk_app->backgrounded;
}

void morpthis_app_back_handled(void) {
	if (sdk_app) {
		sdk_app->back_handled = true;
	}
}

// ---------------------------------------------------------------------------
// log.h
// ---------------------------------------------------------------------------

static char level_char(morpthis_log_level_t level) {
	switch (level) {
	case MORPTHIS_LOG_DEBUG:
		return 'D';
	case MORPTHIS_LOG_INFO:
		return 'I';
	case MORPTHIS_LOG_WARN:
		return 'W';
	case MORPTHIS_LOG_ERROR:
		return 'E';
	default:
		return '?';
	}
}

void morpthis_log(morpthis_log_level_t level, const char *tag, const char *format, ...) {
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	const char *id = sdk_log_id;
	if (tag && tag[0]) {
		fprintf(stderr, "app:%s: [%c] %s: %s\n", id, level_char(level), tag, message);
	} else {
		fprintf(stderr, "app:%s: [%c] %s\n", id, level_char(level), message);
	}
	fflush(stderr);
}
