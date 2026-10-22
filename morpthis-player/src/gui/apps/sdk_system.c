#include "sdk_internal.h"

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

static bool app_screen_hold;

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
	app_screen_hold = hold;
	power_hold_screen_on(hold);
}

// Called from the host's teardown. An app force-closed while it was holding the
// screen must not leave the panel lit for ever.
void sdk_system_app_reset(void) {
	if (app_screen_hold) {
		app_screen_hold = false;
		power_hold_screen_on(false);
	}
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
