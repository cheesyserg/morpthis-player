/*
 * This file is largely LLM-written, be warned
 */

#include "power.h"

// LVGL 9.2 made lv_timer_t opaque. There is a lv_timer_set_period() but no
// getter, and both places here have to read the period before changing it.
#include "lvgl/src/misc/lv_timer_private.h"

#include "src/system/audio/waveform.h"
#include "src/system/device/ota.h"
#include "src/system/device/system.h"

#include <dirent.h>
#include <sys/reboot.h>
#include <sys/stat.h>

#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/headset.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/device/clock.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/device/adb.h"
#include "src/system/library/library.h"
#include "src/system/remote/airplay.h"
#include "src/system/streaming/radio.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/lastfm/lastfm.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/logging.h"
#include "src/system/device/usb.h"
#include "src/system/audio/usbdac.h"
#include "src/system/device/led.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/axpcharge.h"
#include "src/gui/apps/apphost.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/topbar.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// How often the power state machine polls. Kept short so a power-button press
// feels responsive; the work done per tick is trivial.
#define POWER_TICK_MS 250

// How long ago a marker was stamped, from the tick the caller is working with.
//
// A mark can be a fraction ahead of `now`: the tick reads lv_tick_get() once at
// the top and carries that value through, while power_screen_off() -- called
// from inside the very same tick -- stamps its markers with a fresh
// lv_tick_get(). A plain unsigned `now - mark` would then read as forty-nine
// days rather than as minus one, so the difference is taken signed and clamped
// at zero.
static uint32_t since(uint32_t mark, uint32_t now) {
	int32_t elapsed = (int32_t)(now - mark);
	return elapsed > 0 ? (uint32_t)elapsed : 0;
}

// The lowest level the fades step through. The screen-off path goes one step
// further to a true zero through backlight_hold(): the panel is left powered
// while the screen is dark (see power_screen_off), and zero is what makes that
// invisible. The dimmest level the UI *offers* is
// POWER_BRIGHTNESS_MIN_PERCENT (power.h), which is higher; this floor is still
// what the screen-off fade ends at.
#define BRIGHTNESS_MIN 1
#define BRIGHTNESS_MAX_DEFAULT 100

// The two blank levels written to the framebuffer node, spelled out here
// rather than pulled in from linux/fb.h so the host build compiles unchanged.
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4

// Never treat a backlight level below this as a valid "on" level. Guards against
// adopting a leftover dim value -- e.g. a previous run that exited with the
// screen off left the panel at BRIGHTNESS_MIN -- which would make "screen on"
// come back essentially black (backlight technically on, but too dim to see).
#define BRIGHTNESS_ON_FLOOR 20

// The level the backlight is put at for the whole of a wake -- from before the
// unblank, across the panel's own init, until the frame pushed afterwards has
// been scanned out: a true zero.
//
// Zero and not BRIGHTNESS_MIN, although the floor exists so that no *user*
// chooses a level too dim to see. The floor is a level the panel is *looked at
// through*; this one is a level it is never seen at, and a white reset frame
// at the dimmest setting is still a white frame -- which is the flash. Nor
// is a zero the risk a stopped PWM pin is sometimes said to be: the blank the
// driver already performs on this board puts the PWM at zero with the panel
// unpowered behind it, and the screen stays dark for as long as the screen is
// off. A zeroed PWM is therefore demonstrably dark here. Overridable as
// [screen] wake_hold_level in device_config.ini; 1 restores the old
// floor-level hold.
#define WAKE_HOLD_LEVEL_DEFAULT 0

// The wake of this panel, and what the white flash was.
//
// All of this is the wake of a panel that was powered down, which now only
// happens when the dark spell included a sleep: the ordinary screen-off leaves
// the panel powered and its wake is just the backlight (see power_screen_off).
//
// The panel is an SLCD: its glass shows the contents of its own memory, and a
// reset leaves that memory white. Writing 0 to the blank node resets and
// re-initialises the panel and re-arms the controller's scan-out, but the
// write returns before the panel's own init sequence has finished -- sleep-out
// style panels want up to 120 ms of it -- and a frame pushed before the panel
// is listening is simply lost. So a frame that is only painted into the
// framebuffer *before* the unblank leaves the controller nothing to carry to
// the glass: the panel keeps its reset white until something unrelated (the
// clock, a keypress) next happens to paint, and the backlight comes up over
// that white. No amount of holding the backlight down covers a glass that is
// still white when it is released, which is why the longer and longer holds of
// the first attempts did not fix it.
//
// Three things therefore have to hold, in this order (see power_screen_on):
//
//   1. the backlight is at a true zero from before the unblank until the frame
//      has reached the glass (WAKE_HOLD_LEVEL). Nothing is visible while the
//      panel is white, whatever the kernel does on the way through;
//   2. the panel's own init is waited out, still dark, before a frame is
//      pushed to it. Allow the documented 120 ms plus a short margin, so the
//      frame is drawn once, after the panel is ready;
//   3. the hold spans that frame, ending only once the controller has scanned
//      two whole frames out to the panel (FBIO_WAITFORVSYNC, see
//      display_wait_vsync) or the fallback has run out. The hold level is
//      re-asserted every WAKE_HOLD_STEP_MS in case unblank restores its own
//      backlight level along the way.
//
// The vsync answer is only believed when it cost what two scan-outs cost: some
// fb drivers return from FBIO_WAITFORVSYNC at once (or error out only on the
// second call), and counting that as having watched two frames ends the hold
// while the panel is still coming up. A too-fast answer falls through to the
// same long wait as no answer at all. The fallback is short because the panel
// has had its settle and the fresh frame by then -- it stands in for the two
// scan-outs, not for the wake.
#define WAKE_PANEL_SETTLE_MS 120
#define WAKE_PANEL_MARGIN_MS 60
// Two credible vsync waits already prove that the freshly drawn frame has
// reached the panel. Keep a short minimum total hold around that check, but
// avoid the old 90 ms minimum on the normal page-flipping path; unsupported or
// untrusted vsync still gets the longer fallback below.
#define WAKE_HOLD_MIN_MS 50
#define WAKE_HOLD_FALLBACK_MS 150
#define WAKE_HOLD_STEP_MS 15
#define WAKE_VSYNC_FRAMES 2
// Faster than two frames could physically be scanned out: a vsync wait that
// returns within this time was not waiting on any frame.
#define WAKE_VSYNC_PLAUSIBLE_MS 25

// Bluetooth. A firmware whose /etc/init.d/S80_bt_init has not been replaced
// still powers the chip up at every boot -- rfkill on, patchram, bluetoothd,
// bluealsa -- whether anything uses it or not. Putting it back down is
// bluetooth.c's job and not a script's: the firmware's bt_suspend ends with
// `killall dbus-daemon` and would take the bus with it.

// Smooth dim: step size and per-step delay when ramping the backlight down.
// ~17 steps * 12ms ~= 200ms for a full 100->1 fade.
#define FADE_STEP 6
#define FADE_STEP_DELAY_US 12000

// The ramp back up at a wake, over a picture that is already drawn: a few
// steps, so the screen is there almost at once and still does not snap on.
#define WAKE_FADE_STEPS 4
#define WAKE_FADE_STEP_DELAY_US 8000

// --- Module state (owned by the LVGL/main thread unless noted) ---
static power_config_t g_cfg;
static lv_display_t *g_disp;
static lv_timer_t *g_refr_timer; // display refresh timer, paused while screen is off
static long g_max_brightness = -1;
static long g_wake_hold_level = WAKE_HOLD_LEVEL_DEFAULT; // what a wake holds the backlight at
static lv_timer_t *g_power_timer;
static bool g_screen_on = true;
static uint32_t g_screen_off_at; // lv_tick when the panel last went dark

// Whether the panel is blanked, which is not the same question as whether the
// screen is on. A screen-off only puts the backlight out and leaves the panel
// powered and scanning, so that a wake is the backlight returning over a frame
// the panel is already showing. The panel is blanked when the device goes to
// standby, and the following wake then runs the full reset/init sequence. Kept
// by screen_power(), read by the display through power_panel_is_blanked().
static bool g_panel_blanked;

// The standby countdown: the idle wait before a suspend attempt. It restarts at
// every activity -- something playing, a key, the cable going in or out --
// rather than counting from the screen blank. Pause therefore counts as idle
// (the count advances and the device suspends), while a playing track or a
// cable just unplugged clear it. The short gap between one track and the next
// (gapless load) is far too brief to reach the timeout.
static uint32_t g_mem_active_ms;   // last activity tick, for standby purposes
static bool g_usb_present_last;    // cable state on the previous tick, to spot a change
static bool g_usb_present_known;   // false until the cable has been read at least once


static long g_hw_brightness = -1;	   // last brightness value actually written to sysfs
static uint32_t g_last_playing_ms;	   // last tick audio was PLAYING (suspend clock)
static uint32_t g_ignore_button_until; // tick until which power presses are absorbed
static uint32_t g_last_active_boot_ms;	// last activity, on a clock that counts suspended time
static bool g_bt_powered_down;			// the boot-time stack has already been put down

// Cross-thread fields: written by input threads, read by the state machine.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_power_button_pending = false;
static uint32_t g_last_button_activity_ms; // last physical-button/explicit activity

// --- Small sysfs helpers ---
static long read_long_from_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long value = -1;
	if (fscanf(f, "%ld", &value) != 1) {
		value = -1;
	}
	fclose(f);
	return value;
}

static void write_long_to_file(const char *path, long value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		perror("power: open sysfs for write");
		return;
	}
	fprintf(f, "%ld", value); // no trailing newline, matching Rockbox's sysfs writer
	fclose(f);
}

static long brightness_max(void) { return g_max_brightness > 0 ? g_max_brightness : BRIGHTNESS_MAX_DEFAULT; }

// Write a backlight level to sysfs, clamped to [BRIGHTNESS_MIN, max]. No-op on
// the host build (no brightness path), but still tracks the value.
static void backlight_write(long value) {
	long max = brightness_max();
	if (value > max) {
		value = max;
	}
	if (value < BRIGHTNESS_MIN) {
		value = BRIGHTNESS_MIN;
	}
	g_hw_brightness = value;
	if (g_cfg.brightness_path) {
		write_long_to_file(g_cfg.brightness_path, value);
	}
}

static uint32_t monotonic_ms(void);

// The level the backlight is held at across a wake: a true zero by default
// (see WAKE_HOLD_LEVEL_DEFAULT), which is darker than anything the user can
// choose. Written whether or not the tracked value already says so, because
// the unblank path in the kernel writes the hardware without telling anyone.
static void backlight_hold(void) {
	g_hw_brightness = g_wake_hold_level;
	if (g_cfg.brightness_path) {
		write_long_to_file(g_cfg.brightness_path, g_wake_hold_level);
	}
}

// Re-asserts the hold every WAKE_HOLD_STEP_MS for `ms` in total. Used for the
// windows the panel spends doing something of its own -- its init after the
// unblank above all -- during which the kernel may write the backlight itself.
static void backlight_hold_for(uint32_t ms) {
	uint32_t started = monotonic_ms();
	backlight_hold();
	for (;;) {
		uint32_t elapsed = monotonic_ms() - started;
		if (elapsed >= ms) {
			break;
		}
		uint32_t left = ms - elapsed;
		usleep((left < WAKE_HOLD_STEP_MS ? left : WAKE_HOLD_STEP_MS) * 1000u);
		backlight_hold();
	}
}

// Keeps the backlight held until the frame pushed after the unblank is known
// to have reached the glass: until the controller has scanned
// WAKE_VSYNC_FRAMES frames out to the panel, or the fallback delay where the
// controller cannot be asked, and never less than WAKE_HOLD_MIN_MS in all. The
// hold level is re-asserted at every step.
//
// The vsync answer is only believed when it cost what two scan-outs cost:
// some fb drivers return from FBIO_WAITFORVSYNC at once (or error out only on
// the second call), and counting that as having watched two frames ends the
// hold while the panel is still coming up -- which is the white flash coming
// back. A too-fast answer falls through to the same long wait as no answer.
static void backlight_hold_until_panel_shows(void) {
	if (!g_cfg.blank_path) {
		return; // the panel was never powered down: no reset, nothing to wait out
	}
	uint32_t started = monotonic_ms();
	backlight_hold();

	uint32_t vsync_at = monotonic_ms();
	bool vsynced = display_wait_vsync(WAKE_VSYNC_FRAMES);
	uint32_t vsync_took = monotonic_ms() - vsync_at;
	if (vsynced && vsync_took < WAKE_VSYNC_PLAUSIBLE_MS) {
		vsynced = false; // too quick to have seen any frame go by
	}
	backlight_hold();

	uint32_t total = vsynced ? WAKE_HOLD_MIN_MS : WAKE_HOLD_FALLBACK_MS;
	uint32_t elapsed = monotonic_ms() - started;
	if (elapsed < total) {
		backlight_hold_for(total - elapsed);
	}
}

// Smoothly ramp the backlight from its current level to target, mirroring the
// stock/Rockbox dim-to-off feel. Blocking, but only for ~200ms during a screen
// transition (the UI isn't visible mid-transition anyway).
static void fade_to(long target) {
	if (g_hw_brightness < 0) {
		backlight_write(target);
		return;
	}
	int dir = (target > g_hw_brightness) ? 1 : -1;
	while (g_hw_brightness != target) {
		long next = g_hw_brightness + dir * FADE_STEP;
		if ((dir > 0 && next > target) || (dir < 0 && next < target)) {
			next = target;
		}
		backlight_write(next);
		if (g_hw_brightness != target) {
			usleep(FADE_STEP_DELAY_US);
		}
	}
}

// The same ramp in WAKE_FADE_STEPS even steps.
static void fade_up_quickly(long target) {
	long from = g_hw_brightness < 0 ? BRIGHTNESS_MIN : g_hw_brightness;
	for (int i = 1; i <= WAKE_FADE_STEPS; i++) {
		backlight_write(from + (target - from) * i / WAKE_FADE_STEPS);
		if (i < WAKE_FADE_STEPS) {
			usleep(WAKE_FADE_STEP_DELAY_US);
		}
	}
}

// Turn the panel on and off through the framebuffer blank node.
//
// Off is FB_BLANK_POWERDOWN and not FB_BLANK_NORMAL, because the two drivers
// behind the node read the value differently. The display driver only tests
// for non-zero, so either value stops the controller, drops its clock and
// calls the panel's power-off -- reset low, VCC and VCCIO opened at the PMIC.
// The touch driver compares against POWERDOWN exactly, and does nothing at
// all for the other blank levels: with NORMAL the controller kept scanning
// and holding its rail up for a screen nobody could see.
//
// What it does on the way down is its own decision, taken from the gesture
// flag set just before the blank: doze when double-tap wake is on, a full
// power-off when it is not. The backlight follows either way -- its driver
// takes the blank as a reason to put the PWM at zero.
static void screen_power(bool on) {
	if (!g_cfg.blank_path) {
		// No blank control on this platform, so the panel is never blanked --
		// which is what the flag has to say, or the display would skip the
		// pan on a panel that is perfectly able to take it.
		g_panel_blanked = false;
		return;
	}
	g_panel_blanked = !on;
	write_long_to_file(g_cfg.blank_path, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
}

// What screen_power() last wrote. Asked by the display, which must not attempt
// a page flip on a panel that is still blanked (see display_wake_begin).
bool power_panel_is_blanked(void) { return g_panel_blanked; }

// Whether the panel is blanked right now. The node is asked directly instead of
// trusting the flag alone: the framebuffer console's own blanking timer, where
// the kernel has one armed, can power the panel down behind this file's back,
// and the wake has to know or it would bring the backlight up over a glass that
// has been reset. Falls back to the flag when the node cannot be read, and is
// false on a platform with no blank control.
static bool panel_is_blanked_now(void) {
	if (!g_cfg.blank_path) {
		return false;
	}
	long blank = read_long_from_file(g_cfg.blank_path);
	return blank >= 0 ? blank != FB_BLANK_UNBLANK : g_panel_blanked;
}

// Enable/disable every LVGL input device (i.e. the touchscreen). Physical
// buttons live in their own threads and are unaffected, so volume/power keys
// keep working while the screen is off -- only stray touches are ignored.
//
// On the way back, a finger already on the panel is not a tap on the page.
// A double-tap wake fires on the second tap's touch-down, and that tap is
// usually still down when the panel comes back 60 ms later: LVGL would see it
// as a fresh press and click whatever lies under it on release. The position
// it reports is not to be trusted either: one the controller sent in doze, or
// the last touch before the blank, which is often the switch that was just
// turned on (the Wi-Fi transfer page's own, for one), which the wake would
// then turn straight off. lv_indev_wait_release() drops that press up to its
// release; with no finger down it clears on the first read and the next tap
// goes through as usual.
static void set_indevs_enabled(bool enable) {
	lv_indev_t *indev = lv_indev_get_next(NULL);
	while (indev) {
		lv_indev_enable(indev, enable);
		if (enable && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
			lv_indev_wait_release(indev);
		}
		indev = lv_indev_get_next(indev);
	}
}

// CLOCK_BOOTTIME, which -- unlike lv_tick_get()'s CLOCK_MONOTONIC -- keeps
// counting while the SoC is suspended. Everything measured in wall-clock
// terms (the automatic shutdown) has to use this one, or a device that spends
// the night suspended would come back thinking no time had passed.
static uint32_t boottime_ms(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
		return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
	}
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

// current idle time in ms considering both touch (LVGL) and physical buttons
static uint32_t input_idle_ms(uint32_t now) {
	uint32_t touch_idle = lv_display_get_inactive_time(g_disp);

	pthread_mutex_lock(&g_lock);
	uint32_t btn_idle = now - g_last_button_activity_ms;
	pthread_mutex_unlock(&g_lock);

	return touch_idle < btn_idle ? touch_idle : btn_idle;
}

static bool g_screen_on_hold;
static bool g_screen_view_hold; // see power_hold_screen_for_view
static bool g_screen_app_hold;  // see power_hold_screen_for_app; protected by g_lock

static bool screen_app_hold_active(void) {
	pthread_mutex_lock(&g_lock);
	bool hold = g_screen_app_hold;
	pthread_mutex_unlock(&g_lock);
	return hold;
}

// ---------------------------------------------------------------------------
// charge limit
//
// Holding the battery below full, and doing it in a way that cannot leave the
// player unable to charge. The node is not on every firmware build, so when it
// is missing the setting is remembered but cannot be enforced, and the settings
// page says so rather than pretending.
// ---------------------------------------------------------------------------

// How charging is stopped: one node on the charger driver.
//
// Despite its name -- the driver's own property is its step-charging algorithm,
// which is what the bit belongs to -- writing 0 here clears CHG_CONFIG in the
// MP2731's REG04h, and that disables charging while LEAVING THE INPUT PATH
// ACTIVE. The distinction is the whole point:
//
//   * the input current limit (input_current_limited) starves the system as
//     well as the battery. A flat battery behind a throttled input cannot
//     charge AND cannot boot the player that would release it -- which is the
//     soft-brick this replaces;
//   * CHG_CONFIG stops only what goes into the cell. The converter keeps
//     supplying the system from the cable, so the player always starts and
//     mp2731_hw_init resets the charger at every boot.
//
// Measured both ways on the device: with charging disabled the input still
// carried the system (250 mA in, 0 mA into the battery, the system rail held
// 200 mV above the cell), and with the battery physically removed the player
// powered on from the charger alone.
//
// The two nodes under /sys/class/power_supply/usb/ the stock player writes are
// not used at all: that supply is the AXP2101 PMIC, not the charger, and
// writing them does nothing to charging.
#define STEP_CHARGING_NODE "/sys/class/power_supply/mp2731-charger/step_charging_enabled"

static const char *g_charge_node;	 // the writable node, NULL if there is none
static int g_charge_limit = 100;	 // 100 = charge to full
static bool g_charging_suspended;

// Set while something has deliberately forbidden charging regardless of the
// battery level -- DAC mode with the charger switched off. Kept separate from
// g_charging_suspended, which is the automatic limit doing its job: the tick
// must not undo a choice the user made.
static bool g_charging_blocked;


static void find_charge_node(void) {
	static bool searched;
	if (searched) {
		return;
	}
	searched = true;

	if (access(STEP_CHARGING_NODE, W_OK) != 0) {
		printf("power: %s is not writable; the charge limit cannot be enforced\n", STEP_CHARGING_NODE);
		return;
	}

	g_charge_node = STEP_CHARGING_NODE;
	printf("power: charge limit will use %s\n", g_charge_node);
}

bool power_charge_limit_supported(void) {
	find_charge_node();
	return g_charge_node != NULL;
}

int power_get_charge_limit(void) { return g_charge_limit; }

bool power_charging_held(void) { return g_charging_suspended || axpcharge_holding(); }

static void charger_run(bool run);

void power_charging_release(void) {
	find_charge_node();
	if (!g_charge_node) {
		return;
	}
	charger_run(true);
	g_charging_suspended = false;
}

void power_set_charge_limit(int percent) {
	if (percent < 80) {
		percent = 80;
	}
	if (percent > 100) {
		percent = 100;
	}
	g_charge_limit = percent;
}

static void charger_run(bool run) {
	if (g_charge_node) {
		write_long_to_file(g_charge_node, run ? 1 : 0);
	}
}

// Called from the tick, and from anything that changes one of the inputs.
//
// One place decides, and it decides from scratch every time rather than from
// what the last decision was: the charger is written only when the answer has
// changed, so this can be called as often as anything likes.
static void apply_charge_limit(void) {
	find_charge_node();
	if (!g_charge_node) {
		return;
	}

	bool allow;
	if (!usb_vbus_present()) {
		// No cable, nothing to hold off -- and this is the case that must not
		// be got wrong. The bit survives the charger being unplugged, so a
		// player that left it off would meet the next cable with a charger that
		// does nothing, until the next reboot reset it.
		allow = true;
	} else if (g_charging_blocked) {
		// DAC mode with charging switched off. Outranks the percentage limit
		// and outlasts it: the tick must not undo a choice the user made.
		allow = false;
	} else if (g_charge_limit >= 100) {
		allow = true;
	} else {
		const char *level = read_battery_percent();
		if (!level || level[0] < '0' || level[0] > '9') {
			return; // no reading to judge by; leave the charger as it is
		}
		int percent = atoi(level);
		// Three percent of hysteresis, so it does not chatter at the boundary.
		allow = g_charging_suspended ? percent <= g_charge_limit - 3 : percent < g_charge_limit;
	}

	// Written once at startup whatever the answer, so the player's idea of the
	// charger and the charger agree from the first tick rather than from the
	// first change.
	static bool ever_written;
	if (ever_written && !allow == g_charging_suspended) {
		return; // already where it should be
	}
	ever_written = true;

	charger_run(allow);
	g_charging_suspended = !allow;
	printf("power: charging %s\n", allow ? "on" : "held off at the limit");
}

// Forbids or allows charging outright, on top of whatever the percentage limit
// is doing. DAC mode is the one caller: the choice is the user's and the
// percentage limit must not undo it.
void power_set_charging_allowed(bool allowed) {
	find_charge_node();
	g_charging_blocked = !allowed;

	if (!g_charge_node) {
		printf("power: charging cannot be controlled on this firmware\n");
		return;
	}

	printf("power: charging %s\n", allowed ? "allowed again" : "forbidden");
	apply_charge_limit(); // one place decides, including this

}

// ---------------------------------------------------------------------------
// automatic shutdown
// ---------------------------------------------------------------------------

static bool g_auto_off_enabled;
static uint32_t g_auto_off_ms;

// ---------------------------------------------------------------------------
// Why a naive suspend-to-RAM does not work here
//
// Of the whole audio chain, the only module without suspend/resume callbacks is
// soc_aic, the I2S controller -- a block inside the SoC, so precisely the one
// that loses its registers when the SoC powers down. Nothing reprograms it on
// resume, and the first snd_pcm_open opens a device whose driver believes it is
// configured and whose hardware is not, which takes the device down. Reloading
// the module is no way out either: `rmmod soc_aic` followed by `insmod` crashes
// the device even cold, with no suspend involved, and soc_aic.ko exposes no
// power parameters.
//
// So most of the saving comes from cutting wake-ups -- screen off, Wi-Fi
// parked, Bluetooth down, timers paused -- and the
// suspend sequence further down tears the audio context all the way down before
// writing `mem`.
// ---------------------------------------------------------------------------

void power_set_auto_off(bool enabled, uint32_t minutes) {
	g_auto_off_enabled = enabled;
	g_auto_off_ms = minutes * 60u * 1000u;
}


static bool g_double_tap_wake; // armed at every screen-off and every blank; see power_set_double_tap_wake

// ---------------------------------------------------------------------------
// The headphone remote across a suspend
//
// The inline-remote module (see headset.h) reports a volume-up press as the
// device comes back from `mem`, while the audio route is re-initialised, and
// never the release that goes with it. Taken at its word that is a key held
// down: the volume climbs a step every 70 ms, all the way to the top, until the
// headphones are pulled out. So the remote's keys are not listened to from just
// before the write that suspends until a moment after the wake.
//
// Before and not only after: whatever the module reports while resuming is
// queued in the kernel and can be read by the input thread before this one has
// returned from the write.
// ---------------------------------------------------------------------------

#define HEADSET_SETTLE_MS 1500

static int g_headset_sleeping;			// set across the `mem` write
static uint32_t g_headset_settled_at;	// CLOCK_MONOTONIC, ms; 0 before any wake

static uint32_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

bool power_headset_keys_settling(void) {
	if (__atomic_load_n(&g_headset_sleeping, __ATOMIC_ACQUIRE)) {
		return true;
	}
	uint32_t settled_at = __atomic_load_n(&g_headset_settled_at, __ATOMIC_ACQUIRE);
	return settled_at != 0 && (int32_t)(settled_at - monotonic_ms()) > 0;
}

static void headset_keys_sleep(void) { __atomic_store_n(&g_headset_sleeping, 1, __ATOMIC_RELEASE); }

// The window is set before the flag comes down, so there is no moment between
// the two in which a key from the wake would be taken.
static void headset_keys_wake(void) {
	uint32_t at = monotonic_ms() + HEADSET_SETTLE_MS;
	__atomic_store_n(&g_headset_settled_at, at ? at : 1, __ATOMIC_RELEASE);
	__atomic_store_n(&g_headset_sleeping, 0, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Parking the radio
//
// The stock player switches Wi-Fi off before every suspend (0x47f730 ->
// system_if_wifi_turn_off, and it waits up to a second for the confirmation)
// and switches it back on afterwards if the setting says so. An associated
// CYW43438 is 20-40 mA of the standby budget, and the SDIO rail it hangs off
// is deliberately kept alive across a suspend (msc0_keep_power_in_suspend=y),
// so nothing else is going to save it.
//
// Not on every screen-off, though: the screen goes dark after thirty seconds
// and a pocket-check a minute later would find the radio re-associating for no
// reason. Only after the screen has been dark this long with nothing playing.
//
// Sixty seconds, the same wait the suspend uses, and the same wait for both
// radios: a minute after the screen goes dark, anything nobody is using goes
// off -- Bluetooth first, then Wi-Fi, then the SoC itself if the Standby toggle
// allows it. The radios do not wait for that toggle, and with them off the
// suspend becomes possible in the first place.
#define RADIO_PARK_AFTER_MS_DEFAULT (60 * 1000)
static uint32_t g_radio_park_after_ms = RADIO_PARK_AFTER_MS_DEFAULT;

// No settings switch for this: parking is always on, and the two in_use() tests
// below are the only places that decide when not to switch off.
static bool g_wifi_parked; // this module switched it off, so it switches it back on
static bool g_bt_parked;

// Whether what is playing right now comes off the network. The download flags
// tested by wifi_in_use() do not cover it: they are raised while a fetch is
// wanted and dropped as soon as the bytes are in, so an album whose next track
// is already cached looks exactly like an idle device -- and parking the radio
// there means the track after that never arrives. What decides is who owns the
// file being played.
//
// radio_is_playing(), not radio_is_active(): a station merely loaded and
// stopped uses nothing, and counting it as in-use would leave the radio on and
// the automatic shutdown disabled for the rest of the day after one listen.
static bool streaming_playback_active(void) {
	if (radio_is_playing()) {
		return true;
	}
	// A network hold keeps the radios up for a streaming or downloading app.
	// Audio writes alone are not a hold -- a game mixer writes silence all day.
	if (apphost_network_hold()) {
		return true;
	}
	if (audio_get_status() == AUDIO_STATUS_STOPPED) {
		return false;
	}

	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	return qobuzcache_owns(path) || tidalcache_owns(path) || podcastcache_owns(path);
}

// Whether the network is the point rather than an idle drain: an AirPlay
// receiver waiting to be found, a DLNA server, a browser halfway through an
// upload, a station streaming, a Qobuz/Tidal/podcast fetch or a Last.fm request
// in flight.
//
// The *_network_wanted() halves matter as much as the downloading ones: between
// the end of one track and the first bytes of the next there is an HTTPS request
// during which nothing plays and nothing downloads, and parking the radio in
// that gap stops the next track from ever arriving.
static bool wifi_in_use(void) {
	// A scan or a connect in flight counts as use: parking the radio out from
	// under wpa_supplicant mid-association leaves the driver to sort out a
	// teardown it did not expect.
	return wifi_busy() || wifi_scan_running() || streaming_playback_active() || airplay_get_enabled() ||
		   dlna_get_enabled() || sonixlink_get_enabled() ||
		   wifitransfer_get_enabled() || qobuzcache_downloading_id() > 0 ||
		   qobuzcache_network_wanted() || tidalcache_downloading_id() > 0 || tidalcache_network_wanted() ||
		   podcastcache_downloading_id() > 0 || podcastcache_network_wanted() || podcastdl_busy() ||
		   lastfm_network_wanted() || ota_busy();
}

// Pushes the next suspend attempt out by the anti-hammer delay. A radio that has
// just been told to go off is still going off: writing `mem` while the SDIO
// chip's teardown is in flight leaves every suspend failing with the main thread
// stuck in D on wait_on_page_bit. The park and the suspend both fall due sixty
// seconds after the screen goes dark, so without this they would land on the
// same tick.
static void mem_defer_after_radio_change(void);

// Takes Wi-Fi down and records that this module did it, so the same module puts
// it back. Shared by the idle timer below and the suspend path.
static void park_wifi_now(void) {
	if (g_wifi_parked || !wifi_get_enabled()) {
		return;
	}
	g_wifi_parked = true;
	printf("power: Wi-Fi off\n");
	wifi_set_enabled_transient(false);
	mem_defer_after_radio_change();
}

// Bluetooth's own "the point rather than an idle drain": a sink being written
// to, a link that is up at all, a scan or a pairing in flight. A radio with
// nothing on the other end of it is worth exactly as much off as on.
//
// SonixLink switched on counts, as it does for the Wi-Fi radio: a phone can
// reach it over Bluetooth, and a parked radio is one the phone cannot find.
static bool bluetooth_in_use(void) {
	return bluetooth_audio_active() || bluetooth_connected_device(NULL) || bluetooth_scan_running() ||
		   bluetooth_busy() || sonixlink_get_enabled();
}

// Whether the player has anything on. This is what holds the shutdown timer at
// zero, so it is a list of things the player is DOING, which is not the same
// list as "the radio has to stay up".
//
// A pair of headphones merely connected is not on it: a silent link is exactly
// the state the shutdown timer exists for. Nor is an AirPlay or DLNA server
// switched on but idle -- those hold the Wi-Fi radio (see wifi_in_use) without
// holding the device awake, and counting them here would let one forgotten
// toggle disable the automatic shutdown for good.
//
// A key pressed with the screen dark counts. Skipping tracks in a pocket is the
// user doing something, and switching off under their fingers is not what a
// shutdown timer is for.
static bool player_is_busy(bool playing, uint32_t input_idle) {
	return playing ||							   // a track or a station
		   input_idle < POWER_TICK_MS * 4 ||	   // a key just pressed
		   usbdac_is_active() ||				   // a computer feeding the DAC
		   usb_storage_active() ||				   // the card exported over USB
		   library_scan_running() ||			   // a scan, which is also writing to the card
		   audiobookdb_scan_running() || wifitransfer_running() || dlna_running() || airplay_running() ||
		   sonixlink_is_connected() || // a phone driving the player, not a switch left on
		   qobuzcache_downloading_id() > 0 || tidalcache_downloading_id() > 0 || podcastcache_downloading_id() > 0 ||
		   ota_busy(); // a firmware update being fetched
}

static void park_bluetooth_now(void) {
	if (g_bt_parked || !bluetooth_get_enabled()) {
		return;
	}
	g_bt_parked = true;
	printf("power: Bluetooth off\n");
	bluetooth_set_enabled_transient(false);
	mem_defer_after_radio_change();
}

// Everything that holds the Wi-Fi radio up.
//
// The Bluetooth half of it is not a mistake: the two radios sit on one chip.
// Taking Wi-Fi down mid-teardown is the hazard described over park_wifi_now(),
// and `ifconfig wlan0 down` -- which is the whole of the firmware's
// wifi_off.sh -- takes bcmdhd through dhd_stop() on the shared part.
//
// It asks about the LINK and not about sound going over it. With the screen
// already dark and the album finishing, "playing" goes false the instant the
// last track ends, and the very next pass -- 250 ms later -- would otherwise
// park the Wi-Fi radio out from under a live pair of headphones. The cost is
// the Wi-Fi radio staying associated while headphones are connected and quiet;
// the alternative is dropping the link the user is wearing.
static bool wifi_wanted(void) {
	return wifi_in_use() || bluetooth_busy() || bluetooth_audio_active() || bluetooth_connected_device(NULL);
}

// A minute after a radio stops being wanted, it goes off. Not "unless something
// is playing", not "unless the Standby toggle is on" -- a radio with nothing on
// the other end of it is worth exactly as much off as on, and music coming out
// of the jack is no reason to keep the Wi-Fi associated.
//
// The minute runs from the later of two moments: the screen going dark, and the
// radio falling idle. Running it from the screen alone would make the wait
// vanish whenever the two do not coincide -- pausing an internet station an hour
// into a dark screen would take the Wi-Fi down on the next pass. The wait exists
// so that a pause somebody is about to undo does not cost a reassociation.
static uint32_t g_wifi_idle_since;
static uint32_t g_bt_idle_since;

static void park_radios_if_idle(uint32_t now) {
	// Held at now for as long as there is a reason to keep them, so `since`
	// below is "how long has nobody wanted this".
	if (g_screen_on || bluetooth_in_use()) {
		g_bt_idle_since = now;
	}
	if (g_screen_on || wifi_wanted()) {
		g_wifi_idle_since = now;
	}
	if (g_screen_on) {
		return;
	}

	// One radio per pass. Switching a radio off is a job posted to its worker,
	// and the flag this file reads flips before that job has run -- so parking
	// both in the same tick means writing to the SDIO chip while its neighbour's
	// teardown is still in flight, which wedges the suspend path. The next pass
	// is 250 ms away; the second radio can wait for it.
	if (!g_bt_parked && bluetooth_get_enabled() && since(g_bt_idle_since, now) >= g_radio_park_after_ms) {
		fprintf(stderr, "power: parking Bluetooth after %u ms unused\n", since(g_bt_idle_since, now));
		park_bluetooth_now();
		return;
	}

	if (g_wifi_parked || !wifi_get_enabled() || since(g_wifi_idle_since, now) < g_radio_park_after_ms) {
		return;
	}

	fprintf(stderr, "power: parking Wi-Fi after %u ms unused\n", since(g_wifi_idle_since, now));
	park_wifi_now();
}

static void unpark_radios(void) {
	if (g_wifi_parked) {
		g_wifi_parked = false;
		printf("power: Wi-Fi back on\n");
		wifi_set_enabled_transient(true);
	}
	if (g_bt_parked) {
		g_bt_parked = false;
		printf("power: Bluetooth back on\n");
		bluetooth_set_enabled_transient(true);
	}
}

// ---------------------------------------------------------------------------
// Suspend to RAM
//
// The stock firmware's standby writes `mem` 60 seconds after the screen goes
// dark, but only at the end of a precise sequence -- a real player stop, a wait
// for the stopped state, Wi-Fi and Bluetooth already dealt with, and only then
// the write. On resume it reuses nothing: the audio stream restarts from
// scratch (the CS43198's dai_shutdown/dai_prepare redo all the work).
//
// The same sequence is followed here, in the narrowest case:
//
//   * only with nothing playing (stopped or paused): no live music, no radio.
//     Pause is fine -- its PCM is already closed after 5 s and resuming reopens
//     everything from scratch. The only thing that crosses the suspend is RAM,
//     which is preserved.
//   * no live ALSA object: audio_suspend_freeze() also closes the PCM the
//     gapless path holds and waits for the audio thread to let go of everything.
//   * Wi-Fi parked first (an associated chip is 20-40 mA and the network would
//     die in the suspend anyway); with Bluetooth on it does not suspend at all,
//     because its resident daemons have no clean suspend path.
//   * not while a cable is in at all. What it is carrying is reason enough on
//     its own -- the card exported to a host, a computer feeding the DAC, an
//     ADB session -- and a cable that is only charging is reason too: the
//     charge limit is enforced by this file's tick, and the tick does not run
//     while the SoC is suspended.
//   * sync() first: the removable microSD loses power in the suspend
//     (keep_power_in_suspend=0 in the driver's script), so anything bound for it
//     must already have got there.
//
// The wake is the write() returning: the power key and PC28 are wake sources
// registered by the stock drivers. On return the clock and the idle markers are
// realigned, and the output path is forced to re-init so the resumed hardware
// is reprogrammed before the first PCM is opened.
//
// Switched off with [power] standby_mem = 0; the wait is set by
// [power] standby_mem_seconds (60 by default, like the stock firmware).
// ---------------------------------------------------------------------------

static bool g_mem_enabled;
// Raised by a suspend that failed or that came straight back. It stops the
// countdown for THIS dark-screen spell only, and power_screen_on() clears it, so
// standby is never left silently disabled for the rest of the session.
static bool g_mem_blocked_this_dark;
static uint32_t g_mem_after_ms;
static int g_mem_instant_wakes;		// consecutive 0-1 s wakes: something wakes it at once
static int g_mem_instant_wakes_max; // past this many, standby blocks itself
static uint32_t g_mem_last_attempt_ms; // to avoid hammering while a condition blocks
static char g_mem_skip_logged[96];	   // the last skip reason already written to the log

// Standby, toggled at runtime from the power settings. Still tied to
// /sys/power/state being available: on the simulator that path is NULL, so
// standby stays off regardless. Switching it on here also clears a block left by
// a failed suspend, since it has been asked for explicitly.
void power_set_standby_enabled(bool enabled) {
	g_mem_enabled = enabled && g_cfg.power_state_path != NULL;
	g_mem_blocked_this_dark = false;
	g_mem_instant_wakes = 0;
}

// What the kernel says about the last suspend: counters and, above all, the
// device or the step that made the attempt fail. It lives in debugfs, which may
// not be mounted on this firmware, so both known paths are tried.
static void log_suspend_stats(void) {
	static const char *paths[] = {"/sys/kernel/debug/suspend_stats", "/d/suspend_stats"};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *f = fopen(paths[i], "r");
		if (!f) {
			continue;
		}
		char line[128];
		printf("power: mem: %s:\n", paths[i]);
		while (fgets(line, sizeof(line), f)) {
			printf("power: mem:   %s", line);
		}
		fclose(f);
		return;
	}
	printf("power: mem: suspend_stats not readable (debugfs not mounted?)\n");
}

static void mem_defer_after_radio_change(void) { g_mem_last_attempt_ms = lv_tick_get(); }

// One log line per skipped suspend, but only one per reason: on a 250 ms tick
// the same reason would otherwise be logged four times a second for hours.
static void mem_skip(const char *why) {
	if (strncmp(g_mem_skip_logged, why, sizeof(g_mem_skip_logged) - 1) != 0) {
		snprintf(g_mem_skip_logged, sizeof(g_mem_skip_logged), "%s", why);
		printf("power: mem deferred: %s\n", why);
	}
	// Stamping the attempt means the next try is 30 s away, not immediate.
	g_mem_last_attempt_ms = lv_tick_get();
}

// ---------------------------------------------------------------------------
// The RTC wake alarm that carries the automatic shutdown across a suspend.
//
// The process does not run while suspended: a shutdown deadline falling during
// the sleep would go unseen, and the wake (always by the user) restarts the idle
// markers, so with standby on the shutdown would never fire at all. The hardware
// is there, though: the Ingenic RTC is a registered wake source
// (system-power-controller, RTCLDO powered across the suspend).
//
// So before writing `mem`, if the automatic shutdown is on,
// /sys/class/rtc/rtc0/wakealarm is programmed for the moment it would fire. If
// the RTC is what wakes the device (no user: the deadline arrived), it really
// shuts down. If the user wakes it first, turning the screen on disarms the
// alarm, and the next suspend re-arms it with the new deadline.
// ---------------------------------------------------------------------------
#define RTC_WAKEALARM_PATH "/sys/class/rtc/rtc0/wakealarm"

static bool g_rtc_alarm_armed;
static time_t g_rtc_alarm_at;
// The same deadline on CLOCK_BOOTTIME. The wall clock is not usable for this:
// it does not count the sleep, and clock_resync() -- the one thing that would
// fix it -- refuses to do anything until the date has been set by hand or by
// ntpdate, so on a device that has seen neither the deadline check never comes
// true. CLOCK_BOOTTIME counts suspended time and needs nobody's permission.
static uint32_t g_rtc_alarm_boot_at;

static bool rtc_wakealarm_write(const char *val) {
#ifdef HOST_BUILD
	(void)val;
	return false; // never touch the development machine's RTC
#else
	FILE *f = fopen(RTC_WAKEALARM_PATH, "w");
	if (!f) {
		return false;
	}
	fputs(val, f);
	return fclose(f) == 0;
#endif
}

static void rtc_alarm_disarm(void) {
	if (!g_rtc_alarm_armed) {
		return;
	}
	g_rtc_alarm_armed = false;
	rtc_wakealarm_write("0");
	printf("power: RTC wake alarm disarmed\n");
}

static void rtc_alarm_arm_for_auto_off(void) {
	g_rtc_alarm_armed = false;
	// Cleared on every path, not only when a new one is written: an alarm left
	// over from an earlier suspend outlives the setting that armed it, and fires
	// later to wake a device that has no reason to wake -- with the software
	// flag down, so nothing acts on it and it simply burns battery in the dark.
	if (!g_auto_off_enabled || g_auto_off_ms == 0) {
		rtc_wakealarm_write("0");
		return;
	}
	uint32_t idle_wall = boottime_ms() - g_last_active_boot_ms;
	if (idle_wall >= g_auto_off_ms) {
		rtc_wakealarm_write("0");
		return; // already expired: the normal awake tick handles it
	}
	long remaining_s = (long)((g_auto_off_ms - idle_wall) / 1000u) + 5; // margin
	// Cleared first (the kernel rejects a new alarm over a pending one), then
	// the absolute time in epoch seconds, which is how the RTC keeps it.
	if (!rtc_wakealarm_write("0")) {
		return;
	}
	char buf[32];
	time_t at = time(NULL) + remaining_s;
	snprintf(buf, sizeof(buf), "%lld", (long long)at);
	if (!rtc_wakealarm_write(buf)) {
		return;
	}
	g_rtc_alarm_armed = true;
	g_rtc_alarm_at = at;
	g_rtc_alarm_boot_at = boottime_ms() + (uint32_t)remaining_s * 1000u;
	printf("power: mem: RTC wake in %ld s for the automatic shutdown\n", remaining_s);
}

void power_shutdown(void) {
	// Where the music had got to, before anything else stops. The player's own
	// poll writes this only every ten seconds while playing and on a change of
	// state, so without this flush a power-off loses up to ten seconds -- and a
	// position seeked to while paused, which changes no state at all, would
	// never be written.
	device_state_remember_flush();

	// Dark the screen immediately, then fade/close audio before the kernel takes
	// power away from the output stage. In particular, the HBC3000 is parked on
	// its unused socket while its amplifier is still powered, avoiding a pop in
	// the connected headphones during shutdown.
	power_screen_off();
	// The screen-off gesture leaves the panel powered for a quick wake; this
	// exit has no wake, and the power may not go for several seconds.
	power_panel_off();
	// And the status LED, faded out rather than cut: the screen is already dark,
	// so this is the last thing the device says on its way down.
	led_shutdown();
	if (!audio_prepare_poweroff()) {
		fprintf(stderr, "power: audio did not quiesce completely before shutdown\n");
	}

	// The time goes into the RTC before anything else, exactly as the stock
	// player does on its way out: whatever the clock has learned since it was
	// last set is otherwise lost the moment the power goes.
	clock_shutdown();

	// The radio database is on the card: close it so its journal is tidied
	// away before the power goes, rather than left for the next boot to find.
	radio_store_close();

	// Streamed and downloaded tracks are transient and must not survive a
	// power cycle: without this the hidden folders carry a gigabyte of files
	// the user never put there and will not listen to again.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// The charger back on before the power goes: the driver keeps that bit
	// across a shutdown -- mp2731_shutdown writes it from the property it has
	// stored -- and a device put away at its charge limit would meet the next
	// cable with a charger that does nothing.
	power_charging_release();

	// Last, after everything above that writes to the card: a library check
	// running in the background is stopped, the databases closed and the card
	// unmounted.
	storage_release_for_shutdown();
	sync();

	int rc = system("poweroff");
	(void)rc;
	sleep(8);

	// Last resort if init never got there.
	reboot(RB_POWER_OFF);
}

void power_reboot(void) {
	device_state_remember_flush();
	power_screen_off();
	power_panel_off(); // no quick wake follows a reboot
	led_shutdown();
	if (!audio_prepare_poweroff()) {
		fprintf(stderr, "power: audio did not quiesce completely before reboot\n");
	}
	clock_shutdown();
	radio_store_close();
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();
	storage_release_for_shutdown();
	sync();

	// Through init, the same way down as power_shutdown(). The syscall stays as
	// the last resort, on the same eight-second guard, for a firmware whose init
	// never gets there.
	int rc = system("reboot");
	(void)rc;
	sleep(8);
	reboot(RB_AUTOBOOT);
	execl("/bin/sh", "sh", "-c", "reboot", (char *)NULL);
}

// The automatic shutdown, from the normal awake tick and from the RTC alarm
// path just after a resume: the same way down as the power menu's.
static void power_auto_off_now(const char *why) {
	printf("power: automatic shutdown: %s\n", why);
	fflush(stdout);
	power_shutdown();
}

static void suspend_to_ram(void) {
	const char *path = g_cfg.power_state_path;

	printf("power: mem: starting the sequence (screen off for %u s)\n",
		   (unsigned)((lv_tick_get() - g_screen_off_at) / 1000));

	// 1. An app's raw output is not part of the player's own context, and a
	//    paused app holding it open would make the freeze below wait forever.
	//    It is safe here: suspend is only attempted with nothing playing, and
	//    the SDK reopens the same terms on the next play.
	apphost_suspend_prepare();

	// 1-bis. The freeze: a real stop, waited on until the track context is gone --
	//    play_file() returned, decoder and FILE* closed, gapless empty, PCM
	//    closed. Quiescing the ALSA objects is not enough: a paused track keeps
	//    the context alive, the card loses power in the suspend, and the first
	//    play after the wake (an unpause of that context) reboots the device. If
	//    there was a pause, the freeze arms a fresh restart: the next play
	//    rebuilds everything from scratch at the right position.
	if (!audio_suspend_freeze(3000)) {
		mem_skip("the audio context did not stop");
		return;
	}

	// 1-ter. The socket in use muted by the driver, so the amplifier losing
	//        its power is not heard in the headphones.
	audio_park_output_before_suspend();

	// 1-quater. The cable remote's module off, so its bias on the microphone
	//           ring is gone before mem cuts the power under it.
	headset_suspend_prepare();

	// 2. The time written where it survives: the RTC domain stays powered in
	//    suspend (RTCLDO at 1.8 V), but the copy in config is the insurance.
	clock_remember();

	// 3. The radio is already down: suspend_if_idle() switches it off a tick
	//    earlier on purpose -- never write `mem` with the SDIO teardown halfway.

	// 4. The card loses power in suspend, so nothing may be left in flight
	//    towards it.
	sync();

	// 4-ter. The RTC alarm: if the automatic shutdown deadline falls during the
	//        sleep, it is what wakes the device to actually shut down.
	rtc_alarm_arm_for_auto_off();

	// 4-quater. The panel, left powered while the screen was dark so that an
	//           ordinary wake could be immediate. It is powered down here for
	//           the sleep: scanning through `mem` would hold the panel, its
	//           controller and the touch controller up all night, and nobody is
	//           awake to see the reset. The wake from this sleep takes
	//           power_screen_on()'s full sequence.
	power_panel_off();

	// 5. The write that stops the SoC. Blocks until the wake: everything after
	//    it already runs on the other side.
	uint32_t before_wall = boottime_ms();
	printf("power: mem: writing '%s'\n", path);
	fflush(stdout);
	headset_keys_sleep();
	FILE *f = fopen(path, "w");
	if (!f) {
		printf("power: mem: %s will not open (%s) -- prototype off for this session\n", path, strerror(errno));
		g_mem_enabled = false;
		rtc_alarm_disarm(); // armed a moment ago for a sleep that never began
		headset_suspend_finish();
		headset_keys_wake();
		// The route was parked and the DAC muted for a sleep that never began.
		audio_force_output_reinit_after_resume();
		return;
	}
	// The status LED faded out before the sleep, so the light is gone with the
	// screen and comes back with it (led_resume below).
	led_suspend();
	fputs("mem", f);
	int rc = fclose(f);
	int write_errno = errno;
	// Back on before the settle window starts: whatever the module reports as
	// it comes up falls inside the window and is ignored.
	headset_suspend_finish();
	headset_keys_wake();
	uint32_t slept_s = (boottime_ms() - before_wall) / 1000;

	// --- awake from here on ---
	// The line is flushed at once: if the wake ends badly, the log must at least
	// record that the suspend itself succeeded.
	printf("power: mem: awake after %u s (fclose=%d)\n", slept_s, rc);
	fflush(stdout);

	// And the LED back, as a fade-in from the dark it was left in.
	led_resume();

	// There is no "one suspend per dark-screen spell" rule: the countdown simply
	// starts again, like any other countdown, and the instant-wake counter below
	// is what guards against a device that wakes itself in a loop.

	// Before the first play opens a PCM: force the output route to re-init. The
	// suspend powers the HBC3000 down and its kernel resume is a no-op, but the
	// software route survives, so auto_set_output() would see "X == X" and skip
	// hbc3000_enable(). A real X->Y->X on the mixer forces the chip's power-on
	// again, without which the first play after a standby reboots the device.
	//
	// And this is the whole of what a resume owes the audio path. Volume, gain,
	// digital filter, DRE and NOS are not rewritten because they are not lost:
	// the AXP2101 holds every rail at its working voltage through "mem"
	// (suspend_volage == work_voltage on all four DCDCs) and the codec driver's
	// PM callbacks are empty, so the CS43198 keeps its registers. See the note
	// on alsa_controls_reapply() in alsa-controls.c.
	//
	// After a refused suspend too: the route was parked and the DAC muted all
	// the same, and left that way the next play goes to the parked socket in
	// silence.
	audio_force_output_reinit_after_resume();
	if (rc == 0) {

		// And the same courtesy to the radio. The daemons came through the
		// suspend -- they are processes, nothing suspended them -- but the
		// chip under them did not, so without this the stack looks up and hears
		// nothing, and the headphones never come back. A radio that was parked
		// before the suspend is left to unpark_radios(), which rebuilds it
		// anyway.
		if (!g_bt_parked) {
			bluetooth_notify_resume();
		}
	}

	// The verdict. Never insist: a failed suspend followed by a retry a minute
	// later costs thirty seconds of freeze every round, which from outside looks
	// like a device that will no longer switch on. A failing suspend blocks
	// itself and leaves the device usable -- the diagnosis in the log is worth
	// more than an identical second attempt.
	if (rc != 0) {
		printf("power: mem: the suspend FAILED (%s) -- no further attempts while the screen stays off\n",
			   strerror(write_errno));
		log_suspend_stats();
		g_mem_blocked_this_dark = true;
		rtc_alarm_disarm(); // no stray alarm left over from a sleep that never began
		fflush(stdout);
	} else if (slept_s < 2) {
		// Succeeded but lasted no time: something woke it immediately. Once can
		// be chance; several in a row is a wake source that has to be found
		// rather than fought.
		g_mem_instant_wakes++;
		printf("power: mem: instant wake (%d of %d tolerated)\n", g_mem_instant_wakes,
			   g_mem_instant_wakes_max);
		if (g_mem_instant_wakes >= g_mem_instant_wakes_max) {
			printf("power: mem: too many instant wakes -- no further attempts while the screen stays off\n");
			log_suspend_stats();
			g_mem_blocked_this_dark = true;
		}
		fflush(stdout);
	} else {
		g_mem_instant_wakes = 0;
	}

	// The system clock did not count the sleep; the RTC did.
	clock_resync();

	// If the RTC alarm is what woke the device there is no user: the automatic
	// shutdown deadline arrived, so it shuts down here, before the idle markers
	// restart. A user wake before the deadline leaves the alarm armed, and
	// turning the screen on disarms it a moment later.
	//
	// The deadline is read off CLOCK_BOOTTIME rather than the wall clock (see
	// g_rtc_alarm_boot_at), and a queued power press counts as a user: the
	// disarm below only runs on the next tick, so without that test a press
	// within a couple of seconds of the deadline would switch the device off in
	// the user's hand.
	pthread_mutex_lock(&g_lock);
	bool user_woke_it = g_power_button_pending;
	pthread_mutex_unlock(&g_lock);

	if (rc == 0 && g_rtc_alarm_armed && !user_woke_it && (int32_t)(boottime_ms() - g_rtc_alarm_boot_at) >= 0) {
		power_auto_off_now("RTC wake after mem: the idle time has run out");
	}

	// No recovery of the old stream: there is nothing left to recover -- the
	// freeze closed everything before the suspend, and the next play is a fresh
	// start already armed.

	// A full idle wait again before the next sleep, and nothing else. The
	// automatic shutdown's own marker is deliberately left alone: the wake that
	// matters here is the RTC's, which is no sign of life, and restamping it
	// would push the shutdown out by a whole period on every suspend. That
	// shutdown counts on CLOCK_BOOTTIME, which counted the sleep; a real user
	// wake stamps the marker through power_screen_on().
	g_mem_active_ms = lv_tick_get();
	g_screen_off_at = lv_tick_get();
	g_mem_last_attempt_ms = lv_tick_get();
	g_mem_skip_logged[0] = '\0';
}

// Every condition in one place, each of them logged, so the log explains why
// the device is not sleeping.
static void suspend_if_idle(uint32_t now, bool playing) {
	if (!g_mem_enabled || g_mem_blocked_this_dark || !g_cfg.power_state_path || g_screen_on) {
		return;
	}
	if (since(g_mem_active_ms, now) < g_mem_after_ms) {
		return; // not yet the full idle wait (see g_mem_active_ms)
	}
	if (since(g_mem_last_attempt_ms, now) < 30 * 1000) {
		return; // the last attempt gave up: do not hammer
	}
	if (playing) {
		mem_skip("something is playing or holding the device awake");
		return;
	}
	// Pause counts as a stopped state: the PCM is already released after five
	// seconds ("DAC powered down" in the log) and resuming always reopens the
	// device from scratch, route included, which is the path every unpause
	// exercises. What must not cross the suspend is a live ALSA object, not a
	// remembered position: that sits in RAM, and RAM is preserved.
	if (wifi_in_use()) {
		mem_skip("something is using the network");
		return;
	}
	// Busy first, so the log tells the truth: bluetooth_in_use() counts a job in
	// flight as use, and right after the park that job is this file's own.
	if (bluetooth_busy()) {
		mem_skip("Bluetooth is still shutting down");
		return;
	}
	if (bluetooth_in_use()) {
		mem_skip("something is using Bluetooth");
		return;
	}
	if (bluetooth_get_enabled()) {
		// Enabled but idle: the park takes it down now and the suspend goes on
		// the next round, the same way Wi-Fi is handled below.
		park_bluetooth_now();
		mem_skip("Bluetooth just switched off: mem starts on the next round");
		return;
	}
	// The cable is a reason on its own: a player on the charger stays awake
	// until it comes out.
	//
	// The charge limit is what the tick at the bottom of this file enforces, and
	// nothing runs while the SoC is suspended -- a player asleep on the charger
	// charges straight past the limit, and the release point three percent below
	// it is never seen either. Reaching the limit does not lift this: the level
	// falls again on its own, and only something still awake notices.
	//
	// With no limit set there is nothing to enforce, but the answer is the same,
	// because a cable is the one case where sleeping saves nothing that matters.
	if (usb_vbus_present()) {
		mem_skip("the charger is plugged in");
		return;
	}
	// And what the cable is carrying: the card exported over USB, or a computer
	// feeding the DAC, both of which the suspend would cut. Below the cable
	// itself now, and kept because a host can hold the port without VBUS.
	if (usb_storage_active() || usbdac_is_active()) {
		mem_skip("the USB cable is transferring something");
		return;
	}
	// And ADB, which is the same cable carrying something. adbd survives the
	// suspend as a process, but the gadget goes down with the SoC and the host
	// loses the device: a shell left open on a desk dies at the sixty-second
	// mark, and from the outside that is "ADB switches itself off when the
	// screen goes dark". The cable has to be there -- ADB left on with nothing
	// plugged in is a setting, not a connection, and must not keep a device in
	// a pocket awake all night.
	//
	// adb_is_running() walks /proc, so it is asked only once the cable has
	// already answered yes.
	if (usb_vbus_present() && adb_is_running()) {
		mem_skip("ADB is on with the cable plugged in");
		return;
	}
	if (wifi_get_enabled()) {
		// Wi-Fi goes off now, but the suspend waits for a later tick: the SDIO
		// chip's teardown is asynchronous, and writing `mem` right after the
		// radio goes down leaves every suspend failing with the main thread
		// stuck in D on wait_on_page_bit. Thirty seconds between the two gives
		// the driver time to settle.
		park_wifi_now();
		mem_skip("Wi-Fi just switched off: mem starts on the next round");
		return;
	}

	suspend_to_ram();
}

// ---------------------------------------------------------------------------
// What still runs with the panel dark
//
// Pausing the refresh timer and the animation timer at the blank is not enough
// on its own. These keep firing otherwise, and each firing wakes the CPU:
//
//   - LVGL's input-read timer, every 30 ms. lv_indev_enable(false) makes the
//     callback return immediately, but the timer still fires, and every firing
//     is a full pass of lv_timer_handler() and a return trip through the main
//     loop. Thirty-three wake-ups a second to decide there is nothing to read.
//   - the status bar's clock, radio and battery timers, painting a bar nobody
//     can see;
//   - the player page's progress timer, moving a slider nobody can see;
//   - the screensaver's own clock, redrawing a screensaver that is already in
//     the framebuffer and is not being scanned out.
//
// None of them has any effect while the panel is off, because nothing is
// presented. So they are all paused at the blank and started again at the
// wake, where each is also made ready so it runs once immediately rather than
// showing a stale value for a whole period.
//
// A page registers its timer here once and then forgets about it; power.c
// never looks at what the timer does.
// ---------------------------------------------------------------------------
#define STANDBY_TIMERS_MAX 16
static lv_timer_t *g_standby_timers[STANDBY_TIMERS_MAX];
// 0 = paused outright while the panel is dark. Anything else = kept running,
// at that period instead of its own. A timer that only paints can be stopped;
// one that also carries state the device needs in standby -- the battery poll
// drives the status LED, and the LED has to go out on its own after twenty
// seconds and turn red on a charger -- must merely be slowed down.
static uint32_t g_standby_timer_slow_ms[STANDBY_TIMERS_MAX];
static uint32_t g_standby_timer_normal_ms[STANDBY_TIMERS_MAX];
// Whether each one was actually running when the panel went dark. A registered
// timer is not necessarily live -- the screensaver's clock only runs while the
// screensaver is up -- and resuming one that was deliberately stopped would
// quietly start it painting a hidden page for ever.
static bool g_standby_timer_was_running[STANDBY_TIMERS_MAX];
static int g_standby_timer_count;

static void standby_register(lv_timer_t *timer, uint32_t slow_ms) {
	if (!timer) {
		return;
	}
	for (int i = 0; i < g_standby_timer_count; i++) {
		if (g_standby_timers[i] == timer) {
			g_standby_timer_slow_ms[i] = slow_ms; // last word wins
			return;
		}
	}
	if (g_standby_timer_count >= STANDBY_TIMERS_MAX) {
		fprintf(stderr, "power: no room to register another standby timer\n");
		return;
	}
	g_standby_timers[g_standby_timer_count] = timer;
	g_standby_timer_slow_ms[g_standby_timer_count] = slow_ms;
	g_standby_timer_count++;
}

void power_pause_in_standby(lv_timer_t *timer) { standby_register(timer, 0); }

void power_slow_in_standby(lv_timer_t *timer, uint32_t standby_period_ms) {
	standby_register(timer, standby_period_ms ? standby_period_ms : 1);
}

// LVGL's input-read timers, one per input device.
static void indev_timers_set_paused(bool paused) {
	lv_indev_t *indev = lv_indev_get_next(NULL);
	while (indev) {
		lv_timer_t *t = lv_indev_get_read_timer(indev);
		if (t) {
			if (paused) {
				lv_timer_pause(t);
			} else {
				lv_timer_resume(t);
			}
		}
		indev = lv_indev_get_next(indev);
	}
}

static void standby_timers_set_paused(bool paused) {
	for (int i = 0; i < g_standby_timer_count; i++) {
		lv_timer_t *t = g_standby_timers[i];
		if (!t) {
			continue;
		}

		if (g_standby_timer_slow_ms[i] != 0) {
			// Slowed, not stopped.
			if (paused) {
				g_standby_timer_normal_ms[i] = t->period; // opaque type, reached via lv_timer_private.h
				lv_timer_set_period(t, g_standby_timer_slow_ms[i]);
			} else if (g_standby_timer_normal_ms[i]) {
				lv_timer_set_period(t, g_standby_timer_normal_ms[i]);
				lv_timer_ready(t); // catch up at once on the way back
			}
			continue;
		}

		if (paused) {
			g_standby_timer_was_running[i] = !lv_timer_get_paused(t);
			if (g_standby_timer_was_running[i]) {
				lv_timer_pause(t);
			}
		} else if (g_standby_timer_was_running[i]) {
			lv_timer_resume(t);
			lv_timer_ready(t); // do not show a stale clock for a whole period
		}
	}
}

// Called on the UI thread around a screen transition, and returning true if it
// took over the frame. The GUI hangs the screensaver off it; nothing here knows
// what it does.
static bool (*g_wake_hook)(void);

void power_set_wake_hook(bool (*cb)(void)) { g_wake_hook = cb; }

void power_screen_off(void) {
	waveform_set_screen_on(false); // told, but not a reason to stop -- see waveform.h
	if (!g_screen_on) {
		return;
	}
	g_screen_on = false;
	g_screen_off_at = lv_tick_get();
	// The screen going dark opens a new idle window: the standby countdown
	// starts here, unless activity keeps pushing it back.
	g_mem_active_ms = g_screen_off_at;

	// The gesture flag is armed here, as the screen goes dark, so the software
	// double-tap path works during the dark spell; the touch driver reads the
	// same flag when the panel is later blanked for the sleep, and chooses doze
	// or a full power-off from it. Off has to be written too, not merely left
	// as the settings page last put it, or the controller dozes on a night when
	// nothing is listening for the gesture.
	power_set_double_tap_wake(g_double_tap_wake);

	set_indevs_enabled(false); // no touch events into LVGL while the screen is dark
	fade_to(BRIGHTNESS_MIN);   // smooth dim down

	// The panel is deliberately left powered and scanning the frame it already
	// holds; only the backlight is put out. Blanking it here powers the panel,
	// its controller and the touch controller down, and the wake then owes the
	// unblank a reset and the panel's own init before anything can be seen --
	// the quarter second of dark screen between the press and the picture. Left
	// up, the wake is the backlight returning over a frame the panel is already
	// showing. The panel is blanked for the sleep in suspend_to_ram(), where the
	// reset costs nothing that is being waited on, and by
	// panel_off_if_dark_too_long() if the sleep never comes.
	//
	// The true zero, not the fade's floor: the floor is a level the panel is
	// *looked at through*, while this is a level it is never seen at. It is
	// also what the backlight driver restores at the unblank after a sleep
	// (see WAKE_HOLD_LEVEL_DEFAULT). The user's level is not lost: the wake's
	// fade ends at g_cfg.brightness.
	backlight_hold();

	// Draw what the next wake should open on straight into the framebuffer
	// while it is invisible. The panel is still powered and keeps scanning what
	// it is given, so this is literally the picture the next wake shows when
	// the backlight comes back; after a sleep and its unblank it is also the
	// frame the panel presents before the wake repaint, so the page underneath
	// never flashes up before the screensaver. Done here it costs one invisible
	// frame.
	if (g_wake_hook && g_wake_hook()) {
		lv_obj_invalidate(lv_layer_top());
		lv_obj_t *scr = lv_screen_active();
		if (scr) {
			lv_obj_invalidate(scr);
		}
		lv_refr_now(g_disp);
	}

	// Stop rendering entirely while nothing is presented (physical buttons live
	// on their own threads, so they are still handled by the state machine).
	if (g_refr_timer) {
		lv_timer_pause(g_refr_timer);
	}

	// And stop reading input through LVGL. The touchscreen's events still reach
	// the input threads, where a double tap can wake the screen, but nothing may
	// click a page that is not being shown.
	indev_timers_set_paused(true);

	// Everything that only paints: the status bar, the progress slider, the
	// screensaver's clock.
	standby_timers_set_paused(true);

	// And stop the animations with it. LVGL's animation timer runs every ~33 ms
	// for as long as any animation is alive, and the scrolling titles in the
	// player and the control centre are set to repeat for ever -- thirty
	// wake-ups a second computing positions for text nobody can see. They pick
	// up again on the way back.
	lv_timer_t *anim_timer = lv_anim_get_timer();
	if (anim_timer) {
		lv_timer_pause(anim_timer);
	}

	led_set_standby(true); // the LED standby option keys off this

	// The screen going dark is the natural moment to write the time down: a
	// battery pulled out while the device sits in standby then costs a few
	// seconds of clock, not however long was left on the slow timer.
	clock_remember();

	fprintf(stderr, "power: screen off\n");

	// Last, with the screen dark and the rendering timers already down. The app
	// is told after the transition, not in the middle of it, so a callback
	// cannot draw into a refresh that is about to be paused.
	apphost_notify_screen(false);
}

// Paints the whole interface into the framebuffer once: every layer made
// dirty, then an explicit refresh -- which draws even while the refresh timer
// is paused. No display kick: the caller knows whether the panel needs one.
static void repaint_all(void) {
	lv_obj_t *scr = lv_screen_active();
	if (scr) {
		lv_obj_invalidate(scr);
	}
	lv_obj_invalidate(lv_layer_top());
	lv_refr_now(g_disp);
}

// The same inside the display's own wake kick, the one that puts the video
// mode and the scan-out back after a blank (see display_wake_begin).
static void wake_repaint(void) {
	display_wake_begin(g_disp);
	repaint_all();
	display_wake_end(g_disp);
}

void power_screen_on(void) {
	waveform_set_screen_on(true);
	if (g_screen_on) {
		return;
	}
	g_screen_on = true;
	uint32_t wake_started = monotonic_ms();

	// Snapshot before anything unblanks: after a dark spell that went through
	// standby the panel is powered down and the full wake below is owed; after
	// an ordinary screen-off it was left powered and owes only the backlight.
	// The node is asked rather than the flag, so a blank by someone else is not
	// missed (see panel_is_blanked_now).
	bool panel_blanked = panel_is_blanked_now();
	g_panel_blanked = panel_blanked;

	// And disarms the automatic-shutdown RTC alarm: there is a user. The
	// shutdown timer goes back to zero here and stays there until the screen is
	// dark again -- that is what makes it a shutdown timer rather than a
	// stopwatch that happens to notice the screen.
	rtc_alarm_disarm();
	g_last_active_boot_ms = boottime_ms();

	// A new dark spell gets a clean slate: whatever went wrong with the last
	// suspend, the countdown starts again the next time the screen goes off.
	g_mem_blocked_this_dark = false;
	g_mem_instant_wakes = 0;

	// Whatever the screen coming back is for, the radios are probably part of
	// it. Asked for first, because associating and rebuilding a Bluetooth
	// stack take seconds, and they may as well be the seconds the panel spends
	// waking up.
	unpark_radios();

	// The level the screen comes back at is the one the user chose, however dim.
	// No too-dim floor is applied here: that guess belongs at init, where the
	// stored level is adopted from the panel (see power_init). Applied here it
	// would overwrite a deliberate setting on every screen-off/on, while a
	// reboot -- which reads the setting and never passes through here -- would
	// honour it.

	// The clock first: the screen is about to show it, and a screen-off spell
	// may well have included a suspend the system clock did not count.
	clock_resync();

	// Before anything is drawn: whatever wants to own the first frame of the
	// wake (the screensaver) is raised here, while the backlight is still down.
	// Called after the repaint, it would show the page underneath for a split
	// second before appearing. True means it changed what should be on screen,
	// which is the only thing the quick wake has to redraw.
	bool wake_painted = false;
	if (g_wake_hook) {
		wake_painted = g_wake_hook();
	}

	// --- The wake, one of two ways ----------------------------------------
	//
	// A panel left powered by the ordinary screen-off is still scanning the
	// frame drawn as it went dark, so its wake is the backlight coming back
	// over that frame -- plus one repaint if the hook above changed it. A
	// panel that was blanked by a sleep has been reset and powered down, and
	// owes the full sequence below (see WAKE_PANEL_SETTLE_MS).
	//
	// Either way the backlight stays down until the picture is there. Nothing
	// below is meant to be visible before the fade at the end.
	backlight_hold();

	// Resume the timers first, so the one frame we draw below has the right
	// time, battery, playback position and audio-output state.
	standby_timers_set_paused(false);

	// The jack poll is slowed while the screen is dark. Read it synchronously
	// before the repaint, so an unplugged headphone cannot survive in the
	// first status-bar frame after wake.
	topbar_refresh_audio_outputs();

	if (g_refr_timer) {
		lv_timer_resume(g_refr_timer);
	}
	lv_timer_t *anim_timer = lv_anim_get_timer();
	if (anim_timer) {
		lv_timer_resume(anim_timer);
	}

	if (!panel_blanked) {
		// The panel never stopped scanning: the frame drawn at screen-off is
		// already on the glass, under a backlight at zero. Only a picture the
		// hook swapped in needs drawing, and that is done while still dark so
		// the level comes back over the finished frame.
		if (wake_painted) {
			repaint_all();
		}
	} else {
		// Unblank: the panel resets to white, re-initialises and re-arms the
		// controller's scan-out. Keep the backlight held down through that init;
		// unlike an ordinary refresh, the frame has to be pushed after the panel
		// is listening or the SLCD silently loses it.
		screen_power(true);

		// Say so if the unblank lit the backlight on its way through. The read is
		// taken before the hold below overwrites it, and it is the one part of the
		// wake that cannot be seen from the outside: a level restored here is what
		// a white flash under the protection used to be.
		if (g_cfg.brightness_path) {
			long lit = read_long_from_file(g_cfg.brightness_path);
			if (lit > g_wake_hold_level) {
				printf("power: wake: the unblank left the backlight at %ld (hold %ld)\n", lit, g_wake_hold_level);
			}
		}

		// The hold again at once -- then leave the panel dark through its full
		// init window and a small safety margin. Drawing once, after that window,
		// avoids spending two full-screen renders on the single-core audio path
		// while still ensuring the SLCD gets a frame after it is ready.
		if (g_cfg.blank_path) {
			backlight_hold_for(WAKE_PANEL_SETTLE_MS + WAKE_PANEL_MARGIN_MS);
		}
		wake_repaint();

		// Hold at zero until the controller has carried the fresh frame out to
		// the panel -- the moment before this is when the reset white could show.
		backlight_hold_until_panel_shows();
	}

	set_indevs_enabled(true);
	indev_timers_set_paused(false);

	// The picture is on the glass: bring the user's level back over it.
	fade_up_quickly(g_cfg.brightness);
	lv_display_trigger_activity(g_disp);

	led_set_standby(false);

	fprintf(stderr, "power: screen on (brightness=%ld, wake %ums)\n", g_cfg.brightness,
			(unsigned)(monotonic_ms() - wake_started));

	// After the repaint and the fade, so an app that draws on the way back is
	// drawing into a panel that is already showing the current frame.
	apphost_notify_screen(true);
}

void power_toggle_screen(void) {
	if (g_screen_on) {
		power_screen_off();
	} else {
		power_screen_on();
	}
}

bool power_screen_is_on(void) { return g_screen_on; }

// Powers the Bluetooth chip down. Called once at startup, on a player whose
// Bluetooth switch is off: the init scripts leave the whole stack running even
// though nothing here talks to it.
void power_bluetooth_off(void) {
	if (g_bt_powered_down) {
		return;
	}
	g_bt_powered_down = true;
	fprintf(stderr, "power: switching off the Bluetooth the init script left running\n");
	bluetooth_power_down_stack();
}

// The panel is kept powered while the screen is dark so an ordinary wake can be
// immediate, but only for as long as the walk to standby. A still frame left
// behind a dark screen for hours is how a panel picks up image retention, and
// the sleep that would have cut the panel's power never comes while music is
// playing -- playback holds the standby countdown back every tick. So once the
// screen has been dark for the standby wait and the device is still awake, the
// panel is put down anyway. The next wake then takes the full sequence, which
// is the right trade after a dark spell this long.
static void panel_off_if_dark_too_long(uint32_t now) {
	if (g_screen_on || g_panel_blanked || !g_cfg.blank_path) {
		return;
	}
	if (since(g_screen_off_at, now) < g_mem_after_ms) {
		return;
	}
	printf("power: panel off after %u s dark (standby wait %u s)\n",
		   (unsigned)(since(g_screen_off_at, now) / 1000u), (unsigned)(g_mem_after_ms / 1000u));
	power_panel_off();
}

static void power_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// Expire any timed app hold before this tick decides whether the device is
	// busy. The SDK owns the deadline; this is the only thread that applies it.
	apphost_power_tick();
	uint32_t now = lv_tick_get();

	// 1. consume a queued power-button short-press
	bool btn;
	pthread_mutex_lock(&g_lock);
	btn = g_power_button_pending;
	g_power_button_pending = false;
	pthread_mutex_unlock(&g_lock);

	if (btn) {
		if ((int32_t)(now - g_ignore_button_until) >= 0) {
			power_toggle_screen();
		}
		power_notify_activity(); // a power press always counts as activity
		lv_display_trigger_activity(g_disp);
		now = lv_tick_get();
	}

	// 2. music playing keeps resetting the suspend clock (but not the screen
	//    clock -- the screen still dims while a track plays untouched)
	// A live stream or an app's raw output goes out through audio_external_*, which
	// audio.c's own status knows nothing about -- so without the second half of
	// this the automatic shutdown would count a playing radio or an app's music as
	// an idle device and switch it off mid-track.
	//
	// The Bluetooth receiver is the third of those: the music is on a phone and
	// comes in over A2DP, so neither of the first two answers says anything
	// about it. Streaming rather than merely active, because the mode left on
	// with the phone paused is exactly the idle state the timer exists for.
	// An app's raw output is deliberately not in this answer. Games keep a
	// mixer writing silence for their sound effects, and counting those writes
	// as playback kept the device awake for as long as the app was loaded. The
	// app says what it is doing with the power.h hold mask; AWAKE (or NETWORK,
	// which implies it) is the only app-side standby hold.
	bool playing = (audio_get_status() == AUDIO_STATUS_PLAYING) || apphost_awake_hold() ||
				   radio_is_playing() || btreceiver_is_streaming();
	if (playing) {
		g_last_playing_ms = now;
	}

	// 3. idle computations
	uint32_t input_idle = input_idle_ms(now);

	// Any activity pushes the standby countdown back: something playing, a key
	// pressed recently, or the cable changing state. Pause is not activity (the
	// count advances and the device eventually suspends), and the short gap
	// between two tracks is far too brief to reach the timeout, so a suspend
	// cannot fall in the middle of a track change.
	bool usb_now = usb_vbus_present();
	bool usb_changed = g_usb_present_known && usb_now != g_usb_present_last;
	if (playing || input_idle < POWER_TICK_MS * 2 || usb_changed) {
		g_mem_active_ms = now;
	}
	g_usb_present_last = usb_now;
	g_usb_present_known = true;

	// The automatic shutdown is a shutdown timer, and behaves like one: it is
	// held at zero for as long as the screen is on or the player is doing
	// something, and it counts only from the moment the screen goes dark with
	// nothing left to do. Turning the screen on puts it back to zero, wherever
	// it had got to.
	//
	// It counts on CLOCK_BOOTTIME, which keeps running while the SoC is
	// suspended, so a device that spends the wait asleep still switches off at
	// the right moment.
	if (g_screen_on || player_is_busy(playing, input_idle)) {
		g_last_active_boot_ms = boottime_ms();
	}
	uint32_t off_idle_wall = boottime_ms() - g_last_active_boot_ms;

	// 4. auto screen-off after the configured input-idle timeout
	if (g_screen_on && g_cfg.screen_off_enabled && !g_screen_on_hold && !g_screen_view_hold &&
		!screen_app_hold_active() && input_idle >= g_cfg.screen_off_timeout_ms) {
		// Say why. A screen that goes dark on its own is indistinguishable
		// from a crash unless the log records that it was the idle timer.
		printf("power: screen off after %u ms without input (timeout %u ms)\n", input_idle,
			   g_cfg.screen_off_timeout_ms);
		power_screen_off();
	}

	// 4a-bis. the shutdown timer, once it has run out.
	//
	//     Before the suspend, not after it: on a tick where both are ready,
	//     suspending first would leave rtc_alarm_arm_for_auto_off() with a
	//     deadline already past, so it would arm nothing, that sleep would carry
	//     no alarm and the shutdown would slip by a whole period.
	if (g_auto_off_enabled && g_auto_off_ms > 0 && off_idle_wall >= g_auto_off_ms) {
		char why[64];
		snprintf(why, sizeof(why), "%u minutes idle", g_auto_off_ms / 60000u);
		power_auto_off_now(why);
	}

	// 4a-bis-2. the radios, a minute after the screen goes dark
	park_radios_if_idle(now);

	// 4a-bis-3. the real sleep, a minute after the screen goes dark. Stopped and
	//           off the network, the SoC halts the way the stock binary makes it
	//           halt.
	suspend_if_idle(now, playing);

	// The suspend sequence may have closed a paused app's output (or parked a
	// game mixer) before a failed attempt, and a successful one returns here
	// after the wake. The app side reopens whatever it still owns.
	apphost_resume_after_suspend();

	// 4a-bis-4. The panel, if the sleep above did not happen. Left powered behind
	//           a dark screen it is a still frame on a live panel, which is what
	//           image retention comes from -- and with music playing the sleep
	//           never comes at all. See panel_off_if_dark_too_long().
	panel_off_if_dark_too_long(lv_tick_get());

	// 4b. the charger, if it is being held below full: the MP2731 by the
	//     percentage, or the PMIC by its own two switches on the R1
	apply_charge_limit();
	axpcharge_tick();

}

void power_notify_activity(void) {
	pthread_mutex_lock(&g_lock);
	g_last_button_activity_ms = lv_tick_get();
	pthread_mutex_unlock(&g_lock);
}

// On the UI thread: the press is taken now rather than at the timer's next
// tick, up to POWER_TICK_MS away.
static void power_button_now(void *unused) {
	(void)unused;
	if (g_power_timer) {
		lv_timer_ready(g_power_timer);
	}
}

void power_notify_power_button(void) {
	pthread_mutex_lock(&g_lock);
	g_power_button_pending = true;
	pthread_mutex_unlock(&g_lock);
	gui_post(power_button_now, NULL);
}

// --- Runtime configuration ---
void power_set_brightness(long value) {
	long max = brightness_max();
	long floor = power_get_min_brightness();
	if (value > max) {
		value = max;
	}
	if (value < floor) {
		value = floor;
	}
	g_cfg.brightness = value;
	if (g_screen_on) {
		backlight_write(value); // instant (no fade) for a live settings adjustment
	}
}

long power_get_brightness(void) { return g_cfg.brightness; }
long power_get_max_brightness(void) { return brightness_max(); }

long power_get_min_brightness(void) {
	long max = brightness_max();
	long floor = max * POWER_BRIGHTNESS_MIN_PERCENT / 100;
	if (floor < BRIGHTNESS_MIN) {
		floor = BRIGHTNESS_MIN;
	}
	if (floor > max) {
		floor = max;
	}
	return floor;
}

void power_hold_screen_on(bool hold) {
	g_screen_on_hold = hold;
	if (!hold) {
		// The hold was released after minutes of untouched screen (a library
		// scan, the power menu): without resetting the idle clocks here the
		// screen-off timer would fire on the very next tick, blanking the
		// panel the moment the long job announces it has finished.
		power_notify_activity();
		if (g_disp) {
			lv_display_trigger_activity(g_disp);
		}
	}
}

void power_hold_screen_for_view(bool hold) {
	if (hold == g_screen_view_hold) {
		return;
	}
	g_screen_view_hold = hold;
	if (!hold) {
		// As above: the idle timer starts over from the moment the view went,
		// not from the last touch before it came up.
		power_notify_activity();
		if (g_disp) {
			lv_display_trigger_activity(g_disp);
		}
	}
}

// Runs on the GUI thread: a worker may be the one releasing the app's hold, and
// LVGL has no locks here.
static void app_screen_hold_released_cb(void *unused) {
	(void)unused;
	power_notify_activity();
	if (g_screen_on && g_disp) {
		lv_display_trigger_activity(g_disp);
	}
}

void power_hold_screen_for_app(bool hold) {
	pthread_mutex_lock(&g_lock);
	bool changed = g_screen_app_hold != hold;
	g_screen_app_hold = hold;
	pthread_mutex_unlock(&g_lock);
	if (changed && !hold) {
		// As with the other holds: start the idle window when the hold goes,
		// not when the screen was last touched before a long read or job.
		gui_post(app_screen_hold_released_cb, NULL);
	}
}

// Double-tap-to-wake: the touch controller's gesture mode, the same sysfs
// switch the stock player writes (echo on/off > .../gesture_sw, decompile
// FUN_00481fc0). With it on, blanking the panel leaves the controller
// listening for a double tap, which it reports as a power-key press -- the
// button threads then wake the screen exactly as the real power key would.
#define GESTURE_WAKE_NODE "/sys/devices/i2c-1/1-0014/gesture_sw"

bool power_double_tap_wake_enabled(void) { return g_double_tap_wake; }

bool power_double_tap_wake_supported(void) {
	static int answer = -1;
	if (answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		answer = model ? model->tap_wake : access(GESTURE_WAKE_NODE, F_OK) == 0;
	}
	return answer != 0;
}

void power_set_double_tap_wake(bool enabled) {
	if (!power_double_tap_wake_supported()) {
		g_double_tap_wake = false;
		return;
	}
	g_double_tap_wake = enabled;
	FILE *f = fopen(GESTURE_WAKE_NODE, "w");
	if (!f) {
		printf("power: gesture node %s not writable\n", GESTURE_WAKE_NODE);
		return; // host build, or an unexpected firmware: nothing to switch
	}
	fputs(enabled ? "on" : "off", f);
	fclose(f);
	printf("power: double-tap wake %s\n", enabled ? "on" : "off");
}

// Powers the panel down now, rather than leaving it powered the way the
// screen-off gesture deliberately does. For the moments with no quick wake
// ahead: entering standby, and the exits that end in a reboot or a power-off.
// Idempotent.
void power_panel_off(void) {
	if (!g_cfg.blank_path || g_panel_blanked) {
		return;
	}
	// Written first: the touch driver reads this flag as the blank reaches it.
	// The screen-off path has already armed it for a dark spell, but the setting
	// is what the driver must see at the blank, not whatever the node last held.
	power_set_double_tap_wake(g_double_tap_wake);
	screen_power(false);
}

void power_set_screen_off_enabled(bool enabled) { g_cfg.screen_off_enabled = enabled; }
void power_set_screen_off_timeout(uint32_t ms) { g_cfg.screen_off_timeout_ms = ms; }

void power_get_config(power_config_t *out) {
	if (out) {
		*out = g_cfg;
	}
}

void power_init(const power_config_t *cfg, lv_display_t *disp) {
	g_cfg = *cfg;
	g_disp = disp;
	g_refr_timer = disp ? lv_display_get_refr_timer(disp) : NULL;
	g_screen_on = true;

	// discover the panel's maximum backlight level
	g_max_brightness = g_cfg.max_brightness_path ? read_long_from_file(g_cfg.max_brightness_path) : -1;

	// How dark a wake holds the backlight: a true zero by default (see
	// WAKE_HOLD_LEVEL_DEFAULT). [screen] wake_hold_level in device_config.ini
	// overrides it, for a board whose driver misbehaves with the PWM stopped:
	// 1 restores the older floor-level hold.
	g_wake_hold_level = config_get_int("screen", "wake_hold_level", WAKE_HOLD_LEVEL_DEFAULT);
	if (g_wake_hold_level < 0) {
		g_wake_hold_level = 0;
	}
	if (g_wake_hold_level >= BRIGHTNESS_ON_FLOOR) {
		g_wake_hold_level = BRIGHTNESS_ON_FLOOR - 1; // never a level the panel is meant to be seen at
	}

	// if no explicit on-brightness was configured, adopt whatever the panel is
	// currently set to (falling back to max). Never adopt a too-dim value: a
	// prior run may have exited with the screen off, leaving the panel at MIN --
	// adopting that would make the screen appear black once turned "on". A
	// stored value under the UI's floor is treated the same way, as unset.
	if (g_cfg.brightness < power_get_min_brightness()) {
		long cur = g_cfg.brightness_path ? read_long_from_file(g_cfg.brightness_path) : -1;
		g_cfg.brightness = cur >= BRIGHTNESS_ON_FLOOR ? cur : brightness_max();
	}
	screen_power(true); // make sure the panel is unblanked (in case a prior run left it off)
	backlight_write(g_cfg.brightness);


	uint32_t now = lv_tick_get();
	g_last_button_activity_ms = now;
	g_last_playing_ms = now;
	g_ignore_button_until = now;

	g_last_active_boot_ms = boottime_ms(); // the wall-clock idle marker starts now

	// Suspend to RAM: on by default, 60 seconds after the screen goes dark,
	// matching the stock binary's standby.
	// [power] standby_mem = 0 disables it; standby_mem_seconds sets the wait.
	g_mem_enabled = g_cfg.power_state_path && config_get_int("power", "standby_mem", 1) != 0;
	long mem_secs = config_get_int("power", "standby_mem_seconds", 60);
	g_mem_instant_wakes_max = (int)config_get_int("power", "standby_mem_max_instant", 2);
	if (g_mem_instant_wakes_max < 1) {
		g_mem_instant_wakes_max = 1;
	}
	if (mem_secs < 15) {
		mem_secs = 15; // any lower risks the device sleeping under the fingers
	}
	g_mem_after_ms = (uint32_t)mem_secs * 1000u;

	// The radios go down on their own clock, not the Standby toggle's: they are
	// worth switching off whether or not the SoC is going to sleep.
	long park_secs = config_get_int("power", "radio_park_seconds", RADIO_PARK_AFTER_MS_DEFAULT / 1000);
	if (park_secs < 15) {
		park_secs = 15; // any lower and a pocket checked twice re-associates for nothing
	}
	g_radio_park_after_ms = (uint32_t)park_secs * 1000u;

	g_power_timer = lv_timer_create(power_timer_cb, POWER_TICK_MS, NULL);

	printf("power: initialized (max_brightness=%ld, on=%ld, wake_hold=%ld, screen_off=%s/%ums, mem=%s/%lus)\n",
		   g_max_brightness, g_cfg.brightness, g_wake_hold_level, g_cfg.screen_off_enabled ? "on" : "off",
		   g_cfg.screen_off_timeout_ms, g_mem_enabled ? "TEST" : "no", (unsigned long)mem_secs);
}

bool power_radios_parked(void) { return g_wifi_parked || g_bt_parked; }
