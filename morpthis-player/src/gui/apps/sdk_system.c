#include "sdk_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/device/screenshot.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/usb.h"
#include "src/system/playback/device_state.h"

// ---------------------------------------------------------------------------
// system.h
// ---------------------------------------------------------------------------

#define APP_POWER_BITS                                                         \
	(MORPTHIS_POWER_AWAKE | MORPTHIS_POWER_SCREEN | MORPTHIS_POWER_NETWORK)

// The app's hold set, its optional expiry and the suspend callback. The lock
// matters because a hold may be changed from an app worker (downloads, audio)
// while the power timer reads it on the GUI thread.
static pthread_mutex_t app_power_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t app_power_holds;
static uint64_t app_power_deadline_ms;
static morpthis_power_event_cb app_power_event_cb;
static void *app_power_event_user;
static bool app_power_suspended;
// The callback that actually received SUSPENDING. RESUMED is sent only to it:
// an app that replaces its callback across the sleep must not receive an
// unpaired resume.
static morpthis_power_event_cb app_power_suspended_cb;
static void *app_power_suspended_user;

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	// Cast before the divide: a 64-bit divide on MIPS32 is a libgcc call, and
	// tv_nsec is already below one second.
	return (uint64_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u;
}

static void app_power_log(uint32_t old_holds, uint64_t old_deadline, uint32_t holds, uint64_t deadline) {
	if (old_holds != holds || (old_deadline != 0) != (deadline != 0)) {
		morpthis_logi("power", "holds 0x%x%s", (unsigned)holds, deadline ? " (timed)" : "");
	}
}

// Changes the set under the lock, then applies the one hold that lives in the
// player's display code. Applying it under the same lock keeps two app threads
// from leaving the display flag at the older of their two states. The app flag
// is deliberately not power_hold_screen_on(): that one belongs to player pages
// and jobs, and either side releasing it would drop the other's hold.
// `deadline` is only replaced when set_deadline is true: hold_screen()/
// hold_awake() must not silently cancel a timed set_holds_for().
static void app_power_modify(uint32_t set_bits, uint32_t clear_bits, bool set_deadline,
						 uint64_t deadline) {
	pthread_mutex_lock(&app_power_lock);
	uint32_t old_holds = app_power_holds;
	uint64_t old_deadline = app_power_deadline_ms;
	uint32_t holds = (old_holds | (set_bits & APP_POWER_BITS)) & ~clear_bits;
	app_power_holds = holds;
	if (set_deadline) {
		app_power_deadline_ms = deadline;
	}
	uint64_t new_deadline = app_power_deadline_ms;
	bool old_screen = (old_holds & MORPTHIS_POWER_SCREEN) != 0;
	bool new_screen = (holds & MORPTHIS_POWER_SCREEN) != 0;
	if (old_screen != new_screen) {
		power_hold_screen_for_app(new_screen);
	}
	pthread_mutex_unlock(&app_power_lock);

	app_power_log(old_holds, old_deadline, holds, new_deadline);
}

static uint32_t app_power_get(void) {
	pthread_mutex_lock(&app_power_lock);
	uint32_t holds = app_power_holds;
	pthread_mutex_unlock(&app_power_lock);
	return holds;
}

static void copy_string(char *out, size_t size, const char *text) {
	if (!out || size == 0) {
		return;
	}
	snprintf(out, size, "%s", text ? text : "");
}

void morpthis_system_info(morpthis_system_info_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	copy_string(out->device_name, sizeof(out->device_name), sysinfo_device_name());
	copy_string(out->os_version, sizeof(out->os_version), sysinfo_os_version());
	copy_string(out->build_version, sizeof(out->build_version), sysinfo_build_version());
	copy_string(out->serial, sizeof(out->serial), sysinfo_serial_number());
	copy_string(out->dac, sizeof(out->dac), sysinfo_dac_info());
}

bool morpthis_system_storage(morpthis_storage_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	sysinfo_storage_t storage;
	if (!sysinfo_sd_usage(&storage)) {
		return false;
	}
	out->present = storage.present;
	out->total = storage.total;
	out->used = storage.used;
	out->free = storage.free;
	out->used_percent = storage.used_percent;
	return storage.present;
}

int morpthis_system_battery_percent(void) {
	device_state_refresh_battery();
	device_state_t state;
	device_state_get(&state);
	if (state.battery_percent[0] < '0' || state.battery_percent[0] > '9') {
		return -1;
	}
	return atoi(state.battery_percent);
}

bool morpthis_system_charging(void) {
	device_state_refresh_battery();
	device_state_t state;
	device_state_get(&state);
	return state.battery_charging;
}

bool morpthis_system_usb_connected(void) { return usb_vbus_present(); }

bool morpthis_system_mass_storage_active(void) { return usb_storage_active(); }

bool morpthis_system_screen_is_on(void) { return power_screen_is_on(); }

void morpthis_system_screen_on(void) { power_screen_on(); }

void morpthis_system_screen_off(void) { power_screen_off(); }

void morpthis_system_screen_toggle(void) { power_toggle_screen(); }

int morpthis_system_brightness(void) {
	long max = power_get_max_brightness();
	long value = power_get_brightness();
	if (max <= 0 || value < 0) {
		return 0;
	}
	return (int)((value * 100 + max / 2) / max);
}

bool morpthis_system_set_brightness(int percent) {
	long max = power_get_max_brightness();
	if (max <= 0) {
		return false;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	long raw = (max * percent + 50) / 100;
	long floor = power_get_min_brightness();
	if (raw < floor) {
		raw = floor;
	}
	power_set_brightness(raw);
	config_set_int("screen", "brightness", (int)raw);
	config_save();
	return true;
}

void morpthis_system_hold_screen(bool hold) {
	app_power_modify(hold ? MORPTHIS_POWER_SCREEN : 0, hold ? 0 : MORPTHIS_POWER_SCREEN, false, 0);
}

void morpthis_system_hold_awake(bool hold) {
	app_power_modify(hold ? MORPTHIS_POWER_AWAKE : 0, hold ? 0 : MORPTHIS_POWER_AWAKE, false, 0);
}

void morpthis_power_set_holds(morpthis_power_t holds) {
	app_power_modify((uint32_t)holds, APP_POWER_BITS, true, 0);
}

void morpthis_power_set_holds_for(morpthis_power_t holds, uint32_t timeout_ms) {
	uint64_t deadline = timeout_ms ? now_ms() + timeout_ms : 0;
	app_power_modify((uint32_t)holds, APP_POWER_BITS, true, deadline);
}

void morpthis_power_release_all(void) {
	app_power_modify(0, APP_POWER_BITS, true, 0);
}

morpthis_power_t morpthis_power_holds(void) { return (morpthis_power_t)app_power_get(); }

void morpthis_power_set_event_cb(morpthis_power_event_cb cb, void *user) {
	pthread_mutex_lock(&app_power_lock);
	app_power_event_cb = cb;
	app_power_event_user = user;
	pthread_mutex_unlock(&app_power_lock);
}

// NETWORK is deliberately stronger than AWAKE: a parked radio cannot finish a
// transfer. The power manager therefore reads either bit as an awake hold.
bool sdk_system_app_awake(void) {
	uint32_t holds = app_power_get();
	return (holds & (MORPTHIS_POWER_AWAKE | MORPTHIS_POWER_NETWORK)) != 0;
}

bool sdk_system_app_network(void) { return (app_power_get() & MORPTHIS_POWER_NETWORK) != 0; }

void sdk_system_notify_suspend(void) {
	pthread_mutex_lock(&app_power_lock);
	morpthis_power_event_cb cb = app_power_event_cb;
	void *user = app_power_event_user;
	app_power_suspended = cb != NULL;
	app_power_suspended_cb = cb;
	app_power_suspended_user = user;
	pthread_mutex_unlock(&app_power_lock);
	if (cb) {
		cb(MORPTHIS_POWER_SUSPENDING, user);
	}
}

void sdk_system_notify_resume(void) {
	pthread_mutex_lock(&app_power_lock);
	bool was = app_power_suspended;
	morpthis_power_event_cb cb = app_power_suspended_cb;
	void *user = app_power_suspended_user;
	bool still_current = cb && cb == app_power_event_cb && user == app_power_event_user;
	app_power_suspended = false;
	app_power_suspended_cb = NULL;
	app_power_suspended_user = NULL;
	pthread_mutex_unlock(&app_power_lock);
	if (was && still_current) {
		cb(MORPTHIS_POWER_RESUMED, user);
	}
}

void sdk_system_notify_screen(bool on) {
	pthread_mutex_lock(&app_power_lock);
	morpthis_power_event_cb cb = app_power_event_cb;
	void *user = app_power_event_user;
	pthread_mutex_unlock(&app_power_lock);
	if (cb) {
		cb(on ? MORPTHIS_POWER_SCREEN_ON : MORPTHIS_POWER_SCREEN_OFF, user);
	}
}

// The player's power tick. A timed hold is enforced here, not by a thread of
// the app's own, so an app that fills the GUI queue cannot postpone it.
void sdk_system_tick(void) {
	pthread_mutex_lock(&app_power_lock);
	uint32_t old_holds = app_power_holds;
	uint64_t deadline = app_power_deadline_ms;
	if (deadline == 0 || now_ms() < deadline) {
		pthread_mutex_unlock(&app_power_lock);
		return;
	}
	app_power_holds = 0;
	app_power_deadline_ms = 0;
	bool screen = (old_holds & MORPTHIS_POWER_SCREEN) != 0;
	if (screen) {
		// Under the same lock as the set: a hold set by an app thread between
		// the expiry and this call must not have its SCREEN bit cleared here.
		power_hold_screen_for_app(false);
	}
	pthread_mutex_unlock(&app_power_lock);

	app_power_log(old_holds, deadline, 0, 0);
}

// Called from the host's teardown. An app force-closed while it was holding the
// screen must not leave the panel lit for ever, and a stale power hold or
// callback must not survive into the next app.
void sdk_system_app_reset(void) {
	pthread_mutex_lock(&app_power_lock);
	bool screen = (app_power_holds & MORPTHIS_POWER_SCREEN) != 0;
	app_power_holds = 0;
	app_power_deadline_ms = 0;
	app_power_event_cb = NULL;
	app_power_event_user = NULL;
	app_power_suspended = false;
	app_power_suspended_cb = NULL;
	app_power_suspended_user = NULL;
	if (screen) {
		power_hold_screen_for_app(false);
	}
	pthread_mutex_unlock(&app_power_lock);
}

bool morpthis_system_time(int64_t *epoch_seconds) {
	if (!epoch_seconds) {
		return false;
	}
	*epoch_seconds = (int64_t)time(NULL);
	return true;
}

bool morpthis_system_set_time(int year, int month, int day, int hour, int minute) {
	return clock_set(year, month, day, hour, minute);
}

void morpthis_system_screenshot(void) { screenshot_request(); }

void morpthis_system_poweroff(void) { power_shutdown(); }

void morpthis_system_reboot(void) { power_reboot(); }

// ---------------------------------------------------------------------------
// The player's settings store
// ---------------------------------------------------------------------------

bool morpthis_config_get(const char *section, const char *key, const char *fallback,
						 char *out, size_t size) {
	if (!section || !key || !out || size == 0) {
		return false;
	}
	copy_string(out, size, config_get(section, key, fallback));
	return true;
}

long morpthis_config_get_int(const char *section, const char *key, long fallback) {
	return section && key ? config_get_int(section, key, fallback) : fallback;
}

bool morpthis_config_get_bool(const char *section, const char *key, bool fallback) {
	return section && key ? config_get_bool(section, key, fallback) : fallback;
}

void morpthis_config_set(const char *section, const char *key, const char *value) {
	if (section && key) {
		config_set(section, key, value ? value : "");
	}
}

void morpthis_config_set_int(const char *section, const char *key, long value) {
	if (section && key) {
		config_set_int(section, key, value);
	}
}

void morpthis_config_set_bool(const char *section, const char *key, bool value) {
	if (section && key) {
		config_set_bool(section, key, value);
	}
}

bool morpthis_config_save(void) { return config_save(); }
