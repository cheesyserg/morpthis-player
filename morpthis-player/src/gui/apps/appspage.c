#include "appspage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/apps/apphost.h"
#include "src/system/apps/apps.h"
#include "src/system/core/lang.h"

lv_obj_t *appspage_screen;

static lv_obj_t *list;
static lv_obj_t *empty_label;
static app_t apps[APPS_MAX];
static int app_count;

// A UI app is a shared object the SDK host loads into this process. A headless
// tool is still a separate process: the file is not a shared object (or has no
// SDK entry), so the page falls back to fork/execve and waits for it.
#define APPS_LAUNCH_TIMEOUT_MS 30000

typedef struct {
	app_t app;
} launch_job_t;

typedef struct {
	char name[APPS_NAME_MAX];
	apps_result_t result;
} launch_result_t;

static void launch_done_cb(void *user) {
	launch_result_t *done = user;
	char text[APPS_NAME_MAX + 96];

	if (!done->result.started) {
		snprintf(text, sizeof(text), "%s: %s", done->name, done->result.error);
		toast_error(text);
	} else if (done->result.timed_out) {
		snprintf(text, sizeof(text), "%s: timed out", done->name);
		toast_error(text);
	} else if (done->result.signal) {
		snprintf(text, sizeof(text), "%s: signal %d", done->name, done->result.signal);
		toast_error(text);
	} else if (done->result.exit_code != 0) {
		snprintf(text, sizeof(text), "%s: exit %d", done->name, done->result.exit_code);
		toast_error(text);
	} else {
		snprintf(text, sizeof(text), "%s: exit 0", done->name);
		toast_success(text);
	}

	free(done);
}

static void *launch_thread(void *arg) {
	launch_job_t *job = arg;

	launch_result_t *done = calloc(1, sizeof(*done));
	if (done) {
		snprintf(done->name, sizeof(done->name), "%s", job->app.name);
		apps_launch(&job->app, APPS_LAUNCH_TIMEOUT_MS, &done->result);
		if (!gui_post(launch_done_cb, done)) {
			free(done);
		}
	}

	free(job);
	return NULL;
}

static void launch_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= app_count) {
		return;
	}

	// The in-process path first. It runs on the GUI thread because the entry
	// point builds screens; a plugin that cannot start is reported here rather
	// than run as a tool, which would only produce ENOEXEC.
	char error[128];
	apphost_result_t result = apphost_launch(&apps[index], error, sizeof(error));
	if (result == APPHOST_STARTED) {
		return;
	}
	if (result == APPHOST_FAILED) {
		char text[APPS_NAME_MAX + 160];
		snprintf(text, sizeof(text), "%s: %s", apps[index].name, error);
		toast_error(text);
		return;
	}

	// APPHOST_NOT_PLUGIN: a headless tool. The existing worker keeps the GUI
	// responsive while fork/execve runs and waits. The app is copied out of the
	// page's array: the page may be rebuilt while the worker is still starting
	// it, and the row's index would then name a different app.
	launch_job_t *job = malloc(sizeof(*job));
	if (!job) {
		return;
	}
	job->app = apps[index];

	pthread_t thread;
	if (pthread_create(&thread, NULL, launch_thread, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
	}
}

static void rebuild(void) {
	lv_obj_clean(list);
	app_count = apps_scan(apps, APPS_MAX);

	if (app_count <= 0) {
		lv_label_set_text(empty_label, tr(app_count < 0 ? "system_no_card" : "apps_empty_note"));
		lv_obj_set_hidden(empty_label, false);
		return;
	}
	lv_obj_set_hidden(empty_label, true);

	for (int i = 0; i < app_count; i++) {
		lv_obj_t *row = settingsrow_action(list, apps[i].name, launch_cb, (void *)(intptr_t)i);
		settingsrow_add_icon(row, &icon_files_game, THEME_SEMANTIC_GREEN);

		char caption[APPS_VERSION_MAX + APPS_DESC_MAX + 4];
		if (apps[i].version[0] && apps[i].description[0]) {
			snprintf(caption, sizeof(caption), "%s · %s", apps[i].version, apps[i].description);
		} else if (apps[i].version[0]) {
			snprintf(caption, sizeof(caption), "%s", apps[i].version);
		} else {
			snprintf(caption, sizeof(caption), "%s", apps[i].description);
		}
		if (caption[0]) {
			if (apps[i].tier == APP_TIER_HEAVY) {
				char tagged[sizeof(caption) + 8];
				snprintf(tagged, sizeof(tagged), "Heavy · %s", caption);
				settingsrow_add_caption(row, tagged);
			} else {
				settingsrow_add_caption(row, caption);
			}
		}
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void appspage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(appspage_screen, cfg, "apps");

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("apps_note"));

	list = lv_obj_create(container);
	lv_obj_remove_style_all(list);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_height(list, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(list, 8, 0);
	lv_obj_set_scrollable(list, false);

	empty_label = lv_label_create(container);
	lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_label, lv_pct(100));
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_22, 0);
	lv_label_set_text(empty_label, tr("apps_empty_note"));
	lv_obj_set_hidden(empty_label, true);

	lv_obj_add_event_cb(appspage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
