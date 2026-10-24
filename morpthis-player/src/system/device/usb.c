#include "usb.h"

#include "src/system/device/adb.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/bluetooth/btlog.h"
#include "src/system/device/sysserver.h"
#include "src/system/device/system.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/core/utils.h"
#include "src/gui/apps/apphost.h"
#include "src/gui/nowplaying/cover.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"


// The ADB init script (S440adb) builds its own gadget under this directory
// and binds it to the device's one USB controller. ADB and the card reader are
// therefore exclusive: while ADB owns the controller nothing here binds a
// storage gadget, and choosing mass storage turns ADB off first (see apphost).
// The old composite adb+storage experiment is gone; the dwc2 controller on
// this SoC did not tolerate the rebind it needed.
#define ADB_GADGET "/sys/kernel/config/usb_gadget/adb_demo"

// VBUS detection. The first node is what the stock binary polls; the second
// is the charger's own (the stock reads that one too). Which of them actually
// flips to 1 on this hardware differs between drivers, so *all* of them are
// polled and any reading 1 means a cable -- the same any-supply logic the
// battery scan uses, which is why the charge icon worked while a single-node
// poll here saw nothing.
static const char *const VBUS_CANDIDATES[] = {
	"/sys/class/power_supply/usb/online",
	"/sys/class/power_supply/mp2731-charger/online",
};

// How often the cable is looked at. Attaching feels immediate at one second;
// the check is two sysfs reads.
#define POLL_SECONDS 1
// The gadget is unbound while the panel is dark and no cable is in. That is
// exactly when a cable may arrive, and the host must not wait seconds for a
// device to appear: one sysfs read a second is cheap next to a plug-in that
// does nothing.
#define POLL_SECONDS_STANDBY 1

// How long a host that is on the cable gets to enumerate the reader before the
// watcher rebuilds it once. A reader left bound across a host detach can come
// back with dwc2's setup queue armed for the old attach: the host sends SETUP,
// the kernel logs enqueue_setup -EAGAIN, and no configuration is ever reached.
// The ADB on/off workaround was really this UDC toggle, done by hand.
#define ENUM_REBUILD_MS 3000

static char sd_device[128];
static char sd_mount[256];

static volatile bool storage_active;

// After a failed export, leave the card with the player for a moment instead
// of unmounting/remounting it once a second while the same holder is still
// there.
static time_t export_retry_at;

// True once usb_start() has found a card device and a VBUS node. Until then
// there is no gadget to build and no watcher to export anything; the Apps
// guard still records its state so a later start sees it.
static bool usb_started;

// Export and restore run on the watcher thread, and a restore is also asked for
// by the threads that start DAC mode and switch ADB. One at a time: a restore
// landing in the middle of an export would remount the card under a host that
// is about to be handed it.
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

// Configfs gadget construction and binding are not atomic, and more than one
// thread reaches them (the watcher, app teardown, ADB, resume). Two shells
// writing descriptors and UDC at once is how a gadget ends up half-built and
// invisible to the host.
static pthread_mutex_t gadget_lock = PTHREAD_MUTEX_INITIALIZER;

// Ownership model
// ---------------
// Three things can own the one USB controller: DAC mode, ADB, and this file's
// card reader. They are exclusive. DAC mode and ADB are driven from other
// threads; they publish intent in the flags below and, when the card must not
// be exported for even a moment, take it back synchronously before returning.
// The watcher is the only code that *builds or binds* the reader.
//
// The reader is kept on the bus whenever the player might need to be seen (the
// screen is on, or a cable is attached) and no other owner has the controller.
// It is taken off in standby and before mem. Every attach starts with a clean
// UDC toggle when the host has not already enumerated the reader: a gadget left
// bound across an unplug leaves dwc2's setup queue armed for the old host, and
// the next one then sits forever at enqueue_setup -EAGAIN.

// Set when usb_suspend_prepare() had to restore an exported card: the card is
// mounted but the on-card services are still closed, and the wake path reopens
// them.
static bool services_closed_for_suspend;

// What the PC should see in its "device connected" notification: these two
// ids, and the strings below, named after the model this player is running on.
#define USB_ID_VENDOR "0x32BB"
// The product id the stock firmware gives its storage gadget (config.json calls
// it usb_pid, and keeps 0x0004 for the DAC's dac_pid). They were the same number
// here, so a host that had already met this device as a sound card was being
// shown a card reader wearing the same identity.
#define USB_ID_PRODUCT "0x0101"
#define USB_MANUFACTURER "HiBy"

// The product string: the model's name without the maker's, "R3 Pro II" or
// "R1", which is what the stock firmware of each writes.
const char *usb_product(void) {
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return "R3 Pro II";
	}
	const char *name = model->name;
	return strncasecmp(name, "HiBy ", 5) == 0 ? name + 5 : name;
}

// What the LUN answers to a SCSI INQUIRY: vendor (8), product (16), revision
// (4), "HiBy    R3 Pro II U-DISK0100" on the R3. Without it the mass-storage
// function reports "Linux / File-Stor Gadget", which is what a bare gadget
// looks like; the stock writes its own, and some hosts -- Android among them --
// are fussier about a card reader than a PC is.
static void usb_inquiry_string(char *out, size_t out_size) {
	char product[40];
	snprintf(product, sizeof(product), "%s U-DISK", usb_product());
	snprintf(out, out_size, "%-8s%-16.16s0100", USB_MANUFACTURER, product);
}

#define STORAGE_GADGET "/sys/kernel/config/usb_gadget/android0"

bool usb_storage_active(void) { return storage_active; }

// ---------------------------------------------------------------------------
// UI notifications: the worker thread must not touch LVGL, so everything the
// user sees goes through gui_post onto the interface thread.
// ---------------------------------------------------------------------------

static void popup_connected_cb(void *unused) {
	(void)unused;
	gui_notify_popup("usb_storage_shared");
}

static void popup_disconnected_cb(void *unused) {
	(void)unused;
	// Covers the unplug and the PC-side eject alike.
	gui_notify_popup("usb_storage_available_again");
	browser_refresh(); // the PC may have changed the card content
}

static bool vbus_present(void) {
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		char *content = read_file_content(VBUS_CANDIDATES[i]);
		if (!content) {
			continue;
		}
		bool on = content[0] == '1';
		free(content);
		if (on) {
			return true;
		}
	}
	return false;
}

// True while the storage gadget is bound to a controller. Used by the watcher
// to tell "the host has not enumerated yet" from "our gadget fell off".
static bool storage_gadget_bound(void) {
	char *content = read_file_content(STORAGE_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool bound = content[0] != '\0';
	free(content);
	return bound;
}

// True while the ADB gadget exists and is bound to the USB controller (its
// UDC file holds the controller's name). Bound means the controller is busy
// and the mass-storage script's own gadget could never attach.
static bool adb_gadget_bound(void) {
	char *content = read_file_content(ADB_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool bound = content[0] != '\0';
	free(content);
	return bound;
}

// Takes ADB's gadget off the controller without touching the reader. ADB's
// stop path can leave the gadget bound with nothing behind it, and the one
// controller cannot carry two gadgets.
static void adb_gadget_unbind(void) {
	FILE *f = fopen(ADB_GADGET "/UDC", "w");
	if (!f) {
		return;
	}
	fputs("\n", f);
	fclose(f);
}

// ADB owns the controller from the moment its switch is on, not only once
// adbd is running. It may still be starting, and a card reader bound in that
// window keeps ADB's gadget off the controller for good (adb_keep_controller()
// describes the same race at boot). Mass storage is therefore a choice that
// turns ADB off, not something the two share.
static bool adb_owns_controller(void) {
	return adb_switched_on() || adb_is_running();
}

// Set while DAC mode owns the USB controller: nothing in this file may touch
// the gadget then. Without it the watcher thread sees a cable, calls
// ensure_storage_gadget() and rebuilds the mass-storage gadget straight over
// the audio one, taking the stream with it and handing the computer a card it
// should not have. Honoured in the two places that build or bind anything.
static volatile bool dac_owns_usb;

// Set while the Apps feature is using the card. The medium stays with the
// player; the empty reader stays on the bus so the watcher can tell a host from
// a charger and offer to close the app. usb_set_apps_mode() takes a card that
// is already exported back before returning.
static volatile bool apps_own_usb;

// Set across the suspend hand-off, from the unbind in usb_suspend_prepare() to
// usb_resume_restore(). The watcher stands down meanwhile: it sleeps for up to
// a poll interval, and a rebind landing between the unbind and the write to
// /sys/power/state leaves a gadget on the controller, which is what makes this
// SoC reset instead of suspending.
static volatile bool suspend_in_progress;

// Set on resume so the reader is not pre-positioned before the first cable.
// After mem, binding before the host arrives leaves dwc2's setup queue armed
// for the old attach. ADB is gated by VBUS instead, so it too is never bound
// without a cable. Cleared by the watcher when VBUS appears.
static volatile bool resume_wait_cable;

// True when the gadget's configfs tree may not describe the card reader: never
// built yet, DAC mode wrote its audio descriptors into the same tree, or ADB's
// stop tore configfs down. The next attach runs the full builder then; a clean
// tree only needs the cheap UDC toggle.
static volatile bool gadget_needs_build = true;

// The predicate every bind has to pass. It is checked at the top of the builder
// and again inside gadget_rebind_locked(), under gadget_lock, so a transition
// that happens while a shell or a 120 ms settle sleep is in flight cannot land
// the reader on a controller that has just been given to DAC mode, ADB, a USB
// audio device, or the suspend path.
static bool reader_may_bind(void) {
	return !dac_owns_usb && !usbaudio_active() && !adb_owns_controller() && !suspend_in_progress;
}

// Entering and leaving DAC mode moves the card back and forth exactly the way
// a cable being plugged in does, which would announce "Memoria di nuovo
// disponibile" or "Collegato al PC" for something the user did not do and
// cannot see. The popups are silenced for the duration of the transition.
static volatile bool suppress_popups;
static void ensure_storage_gadget(void);
static void ensure_storage_gadget_locked(void);
static bool gadget_unbind(void);
static bool gadget_rebind(void);
static bool first_udc(char *out, size_t out_size);
static void storage_restore(bool reopen_services);
static void storage_reopen_services(void);
static void take_card_back(const char *why);

// Monotonic milliseconds for the enumeration watchdog. time() is not used: the
// clock can be set while the device runs, and a jump must not look like a host
// that has been waiting.
static uint32_t monotonic_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u);
}

void usb_set_dac_mode(bool on) {
	// The flag goes up before anything else: setting it afterwards leaves a
	// window in which the watcher can wake, see a cable and rebuild the
	// reader over the audio gadget this call is about to install.
	suppress_popups = true;
	if (on) {
		dac_owns_usb = true;
		if (usb_started) {
			// DAC mode is a sound card and nothing else. The card comes back
			// before the audio function is built, and the reader comes off the
			// controller because usbdac binds its gadget to the same one.
			take_card_back("DAC mode takes the controller");
			// usbdac rewrites the descriptors in this same tree; the reader must
			// run the full builder before it binds again.
			gadget_needs_build = true;
			gadget_unbind();
			// DAC mode takes the controller from ADB too; a bound adb_demo would
			// make usbdac's own bind fail with EBUSY.
			if (adb_gadget_bound()) {
				adb_gadget_unbind();
			}
		}
	} else {
		// usbdac tears its gadget down before calling this. The watcher
		// attaches the reader again on its next poll if a host is still on the
		// cable; doing it here would race that teardown. DAC rewrote the
		// descriptors (and may have removed the LUN link), so the next attach
		// has to run the full builder even if it was clean before.
		gadget_needs_build = true;
		dac_owns_usb = false;
	}
	fprintf(stderr, "usb: controller %s\n", on ? "handed to DAC mode" : "back from DAC mode");
	suppress_popups = false;
}

bool usb_dac_mode(void) {
	return dac_owns_usb;
}

void usb_set_apps_mode(bool on) {
	if (apps_own_usb == on) {
		return;
	}

	apps_own_usb = on;

	if (!usb_started) {
		return;
	}

	if (on) {
		// An app runs from a package on the card, so a card already handed to a
		// host has to come back before the app can be loaded. Only the medium:
		// the empty reader stays on the bus, which is how a PC is told from a
		// charger and how the watcher knows to ask.
		suppress_popups = true;
		take_card_back("apps are using the card");
		suppress_popups = false;
	}

	// Clearing the guard needs no action: the watcher exports on its next poll
	// if a configured host is still there. With ADB on it stays out of the way;
	// choosing storage turns ADB off first (see apphost).
	fprintf(stderr, "usb: mass storage %s while apps are in use\n", on ? "disabled" : "enabled");
}

// ---------------------------------------------------------------------------
// The Type-C role
//
// This is a dual-role port: the board carries a FUSB302B on the CC lines
// (module_driver/fusb302b.sh loads it with otg_id_gpio=PB24) and an MP2731 that
// can push 5 V back out, so the socket can be either end of a cable. Left to
// itself the controller advertises both and negotiates, which against a PC is
// no contest -- a PC only ever sources -- and this device lands as the
// peripheral every time.
//
// Against a phone it is a contest, and one this device tends to win. A phone is
// dual-role too and Android implements Try.SNK, meaning it would rather be the
// sink; so the two agree that the DAP is the host, the boost turns on, the DAP
// charges the phone, and the card is never offered to anything. From the user's
// side: "nothing happens". The stock player has exactly one function for this
// (set_usb_working_mode at 0x481f20 in hiby_player): it writes "sink" or "dual"
// to /sys/class/typec/port0/port_type and re-applies the saved choice at every
// boot, which is the "USB working mode: Device / Host" item in its menu.
//
// The host half is in use as well: USB-C headphones and USB DACs live on it
// (usbaudio.c). So the role is not pinned -- it is arbitrated, in
// arbitrate_port() over there, and the rule is that the port is dual while it
// is empty and becomes sink again a few seconds after something attaches that
// does not turn into a sound card. A phone is exactly that case; a port pinned
// dual for good could never connect to one.
//
// This function is the sink half of that rule, kept here because it also runs
// before the gadget is bound: it is the role that decides whether the
// controller has a gadget side at all.
//
// The R1 has none of this: no FUSB302B and no Type-C class, but a TCS1421 set
// through its own platform attribute. Its role is looked after entirely by the
// arbitration in usbaudio.c, and this function does nothing there.
// ---------------------------------------------------------------------------
static void typec_force_sink(void) {
	static bool announced;

	// Not while a DAC is being driven. This one line is the whole of the "the
	// sound stops a few seconds after the screen comes back on" bug: waking the
	// panel makes the loop below rebuild the storage gadget, rebuilding the
	// gadget writes "sink" here, and writing "sink" ends the host session the
	// headphones are living in. The port belongs to usbaudio.c while there is
	// something on it.
	if (usbaudio_active()) {
		return;
	}

	for (int port = 0; port < 4; port++) {
		char path[64];
		snprintf(path, sizeof(path), "/sys/class/typec/port%d/port_type", port);
		if (access(path, W_OK) != 0) {
			continue;
		}

		FILE *f = fopen(path, "w");
		if (!f) {
			continue;
		}
		bool ok = fputs("sink", f) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
		if (!announced) {
			announced = true;
			fprintf(stderr, "usb: %s <- sink (%s)\n", path, ok ? "ok" : strerror(errno));
		}
		return;
	}

	if (!announced) {
		announced = true;
		fprintf(stderr, "usb: no writable /sys/class/typec/portN/port_type; leaving the role alone\n");
	}
}

// Writes the controller name (or an empty line) into the gadget's UDC
// attribute. A successful write() is not proof that the driver accepted it:
// the refusal surfaces at close, which is why the callers below read the
// attribute back instead of trusting a single syscall.
static bool gadget_write_udc_locked(const char *text) {
	FILE *f = fopen(STORAGE_GADGET "/UDC", "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", text) >= 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	return ok;
}

// Under gadget_lock. Takes the reader off the controller and verifies that the
// UDC attribute really went empty; a bind that follows while the driver still
// holds the old gadget fails with EBUSY/ENODEV.
static bool gadget_unbind_locked(void) {
	for (int attempt = 0; attempt < 4; attempt++) {
		if (!storage_gadget_bound()) {
			return true;
		}
		gadget_write_udc_locked("");
		usleep(100 * 1000);
	}
	return !storage_gadget_bound();
}

// Under gadget_lock. Binds the reader and waits until configfs reports it,
// rather than assuming a write that returned success has reached the driver.
static bool gadget_rebind_locked(void) {
	if (!reader_may_bind()) {
		return false;
	}
	char udc[NAME_MAX + 1];
	if (access(STORAGE_GADGET "/UDC", W_OK) != 0 || !first_udc(udc, sizeof(udc))) {
		return false;
	}
	for (int attempt = 0; attempt < 5; attempt++) {
		if (storage_gadget_bound()) {
			return true;
		}
		gadget_write_udc_locked(udc);
		usleep(120 * 1000);
	}
	return storage_gadget_bound();
}

// Builds the storage gadget -- descriptors with this device's real name, a
// mass-storage function with no medium in it -- and binds it to the USB
// controller. Idempotent: every step checks before it acts, so this both
// creates the gadget at startup and rebuilds it after ADB tore the configfs
// down.
//
// It is bound with an empty medium because a bound gadget is what lets the
// controller's state tell a PC from a wall charger: a PC enumerates the
// medium-less card reader and the UDC state reads CONFIGURED, a charger only
// supplies power and never does. Exporting is then just writing the block
// device into lun.0/file (the medium clicks in); un-exporting writes it empty
// again.
static void ensure_storage_gadget_locked(void) {
	// apps_own_usb is deliberately not checked here. The guard keeps the
	// *medium* out of the host (storage_export() enforces that); it must not
	// keep the empty card reader off the bus. A bound reader is how the watcher
	// recognises that a PC -- not a charger -- is attached, which is the only
	// way the "close the apps?" prompt can ever be shown. Refusing to bind
	// while an app was alive is how a second plug-in used to do nothing at all.
	if (!reader_may_bind()) {
		return;
	}

	// Already built and bound: nothing to do. The shell below forks the whole
	// player, and this is the common path when a reader is pre-positioned and a
	// host has just enumerated it.
	if (storage_gadget_bound() && !gadget_needs_build &&
		access(STORAGE_GADGET "/functions/mass_storage.0", F_OK) == 0 &&
		access(STORAGE_GADGET "/configs/c.1/mass_storage.0", F_OK) == 0) {
		return;
	}

	bool was_bound = storage_gadget_bound();

	// The role first, every time: the gadget below is only worth binding to a
	// controller that is going to be the peripheral end of the cable.
	typec_force_sink();

	char inquiry[32];
	usb_inquiry_string(inquiry, sizeof(inquiry));
	const char *product = usb_product();

	char cmd[2048];
	// The descriptors are written every time the gadget is about to be bound,
	// not only when it is created. DAC mode builds its audio function in this
	// same gadget (usbdac.c) and leaves its own class, configuration name and
	// power draw behind; a card reader bound over those looks to the host like
	// an audio device with a disk in it, and a phone gives up on it.
	snprintf(cmd, sizeof(cmd),
			 "[ -d /sys/kernel/config/usb_gadget ] || mount -t configfs none /sys/kernel/config; "
			 "mkdir -p %s/strings/0x409 %s/configs/c.1/strings/0x409 && cd %s && "
			 "if ! grep -q '[a-zA-Z0-9]' UDC; then "
			 "echo 0x00 > bDeviceClass; "
			 "echo 0x00 > bDeviceSubClass; "
			 "echo 0x00 > bDeviceProtocol; "
			 "echo 0x0200 > bcdUSB; "
			 "echo 0x0100 > bcdDevice; "
			 "echo %s > idVendor; "
			 "echo %s > idProduct; "
			 "echo '%s' > strings/0x409/manufacturer; "
			 "echo '%s' > strings/0x409/product; "
			 "echo '%s %s' > strings/0x409/serialnumber; "
			 "echo 500 > configs/c.1/MaxPower; "
			 "echo 0x80 > configs/c.1/bmAttributes; "
			 "echo storage > configs/c.1/strings/0x409/configuration; "
			 "for l in configs/c.1/*; do "
			 "[ -L \"$l\" ] && [ \"$l\" != configs/c.1/mass_storage.0 ] && rm -f \"$l\"; "
			 "done; "
			 "fi; "
			 "if [ ! -d functions/mass_storage.0 ]; then "
			 "mkdir functions/mass_storage.0; "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 // Not every kernel has this attribute, and the shell reports a
			 // redirection it cannot make itself -- past the 2>/dev/null, which
			 // only silences the command -- so a kernel without it printed
			 // "can't create ...: Permission denied" into the log at every
			 // setup. Writing it only where it exists keeps the log clean on
			 // both kinds.
			 "[ -e functions/mass_storage.0/lun.0/inquiry_string ] && "
			 "echo '%s' > functions/mass_storage.0/lun.0/inquiry_string; "
			 "fi; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1",
			 STORAGE_GADGET, STORAGE_GADGET, STORAGE_GADGET, USB_ID_VENDOR, USB_ID_PRODUCT, USB_MANUFACTURER,
			 product, USB_MANUFACTURER, product, inquiry);
	int rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "usb: storage gadget setup exited %d\n", rc);
	} else if (!was_bound && reader_may_bind()) {
		// The descriptors and the function are in place now; a later attach
		// only needs the cheap UDC toggle. When the reader was still bound the
		// shell skipped the descriptor block, so the tree is not necessarily
		// clean and the flag stays up. A transition that took the controller
		// while the shell ran (DAC writes the same tree) also leaves it up.
		gadget_needs_build = false;
	}
	// The shell is slow; the audio side can claim the controller while it
	// runs. Do not bind the reader over the gadget it just built.
	if (!reader_may_bind()) {
		return;
	}
	if (!gadget_rebind_locked()) {
		fprintf(stderr, "usb: storage gadget did not bind to the controller\n");
	}
}

static void ensure_storage_gadget(void) {
	pthread_mutex_lock(&gadget_lock);
	ensure_storage_gadget_locked();
	pthread_mutex_unlock(&gadget_lock);
}

// The one clean attach sequence: take the reader off the controller, let the
// driver settle, and build/bind it again with the host watching. A reader left
// bound across an unplug comes back with dwc2's setup queue armed for the old
// attach; this toggle is what clears it, and it is the sequence the old ADB
// on/off workaround performed by hand.
static void reader_reattach(void) {
	if (!reader_may_bind()) {
		return;
	}
	if (adb_gadget_bound()) {
		adb_gadget_unbind();
	}
	typec_force_sink();
	if (!gadget_unbind()) {
		// The gadget is still on the controller; rebinding it now would be a
		// no-op and leave the stale setup queue in place. The next poll tries
		// again.
		return;
	}
	usleep(120 * 1000);
	// The shell that writes descriptors forks the whole player, which on this
	// SoC is expensive next to a playing track. Only run it when the gadget is
	// missing or was left dirty; the plain re-bind is the same UDC toggle and
	// is enough to clear the setup queue.
	if (!gadget_needs_build && access(STORAGE_GADGET "/functions/mass_storage.0", F_OK) == 0 &&
		access(STORAGE_GADGET "/configs/c.1/mass_storage.0", F_OK) == 0) {
		if (reader_may_bind() && gadget_rebind()) {
			return;
		}
	}
	ensure_storage_gadget();
}


// True when a HOST has actually enumerated the bound gadget -- the check that
// tells a PC apart from a wall charger. VBUS alone cannot: a charger raises
// it too, and exporting on VBUS meant every plug-in-to-charge stopped the
// music and unmounted the card mid-listen.
// The name of the one USB device controller, or false when there is none.
static bool first_udc(char *out, size_t out_size) {
	DIR *dir = opendir("/sys/class/udc");
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "%s", de->d_name);
		found = true;
	}
	closedir(dir);
	return found;
}

static bool udc_configured(void) {
	char *content = read_file_content("/sys/class/udc/13500000.otg_new/state");
	if (!content) {
		// Firmware revision with another controller name: take the first one.
		//
		// By hand, not with popen. This runs once a second for as long as the
		// screen is on and there is no cable, and popen forks the whole player
		// to run a shell -- fifty-odd megabytes of page tables on a device with
		// fifty-six, once a second, next to a thread that has to hand the card
		// a period of audio on time. A directory and a small read cost nothing.
		char name[NAME_MAX + 1];
		if (!first_udc(name, sizeof(name))) {
			return false;
		}
		char path[NAME_MAX + 32];
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", name);
		content = read_file_content(path);
		if (!content) {
			return false;
		}
		content[strcspn(content, "\r\n")] = '\0';
		bool configured = strcasecmp(content, "configured") == 0;
		free(content);
		return configured;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool configured = strcasecmp(content, "configured") == 0;
	free(content);
	return configured;
}

// Takes the storage gadget off the USB controller. Used both to hand the
// controller to ADB and to stop paying for the PHY while the device sits in
// standby with no cable. The write and its verification are the same ones
// ensure_storage_gadget_locked() uses.
static bool gadget_unbind(void) {
	pthread_mutex_lock(&gadget_lock);
	bool ok = gadget_unbind_locked();
	pthread_mutex_unlock(&gadget_lock);
	if (!ok) {
		fprintf(stderr, "usb: could not take the storage gadget off the controller\n");
	}
	return ok;
}

// Puts the gadget back on the controller. False when there is no gadget to put
// back -- it has never been built, or something removed it -- and the caller
// falls back to building one.
static bool gadget_rebind(void) {
	pthread_mutex_lock(&gadget_lock);
	bool ok = gadget_rebind_locked();
	pthread_mutex_unlock(&gadget_lock);
	return ok;
}

// Called right before ADB's init script starts: two gadgets cannot bind at
// once.
//
// A card handed to the host comes back first. With ADB on the card stays with
// the player (see the watcher), and unbinding under an exported medium left it
// unmounted here and gone from the host, until the cable was pulled.
void usb_gadget_yield_to_adb(void) {
	if (!usb_started) {
		return;
	}
	if (dac_owns_usb) {
		fprintf(stderr, "usb: DAC mode owns the controller; ADB cannot take it\n");
		return;
	}
	take_card_back("ADB is starting");
	gadget_unbind();
}

// Called after ADB shuts down. Its stop script also unmounts the whole
// configfs, so the gadget is rebuilt from nothing by the watcher.
//
// A card that was riding on ADB's gadget comes back first. The watcher then
// attaches the reader again if a host is on the cable.
void usb_gadget_reclaim_from_adb(void) {
	take_card_back("ADB stopped");
	// ADB's stop path can leave its gadget bound with nothing behind it, and
	// the one controller cannot carry two gadgets. Its script may also have
	// torn configfs down, so the next attach runs the full builder.
	gadget_needs_build = true;
	if (!adb_is_running() && adb_gadget_bound()) {
		adb_gadget_unbind();
	}
	if (usb_started) {
		fprintf(stderr, "usb: ADB stopped; the watcher will rebuild the reader if a host is there\n");
	}
}

// ADB switched on and adbd up, and yet its gadget is not on the controller:
// the card reader's is, or nothing is. Hands the controller over.
//
// It happens at boot. With the switch left on, adbd is started first, by the
// firmware's script, and it binds its gadget on its own a moment later (on
// the R1 adbd runs /sbin/usb_adb_enable.sh once its endpoints are open). The
// card reader's gadget is built during that moment, takes the one controller,
// and adbd's bind fails for good: a PC then finds an empty card reader and no
// ADB, until the switch is turned off and on again.
//
// Checked on every poll, and cheap when all is well: one read of the ADB
// gadget's UDC file. adbd is looked for in /proc only when that is empty, and
// nothing is written unless the gadget is ready to bind (its functionfs is
// mounted, which adbd needs before it can write its descriptors).
static void adb_keep_controller(void) {
	if (!adb_switched_on() || adb_gadget_bound() || access(ADB_GADGET "/functions/ffs.adb", F_OK) != 0) {
		return;
	}
	if (!adb_is_running()) {
		return;
	}

	char udc[NAME_MAX + 1];
	if (!first_udc(udc, sizeof(udc))) {
		return;
	}

	// Under the same lock the reader builder uses, and re-checking every owner:
	// a suspend or DAC transition that starts while this waits must not have
	// its unbind undone by an ADB bind. usb_suspend_prepare() sets its flag
	// before taking this lock and then unbinds every gadget, so an ADB bind
	// that wins the lock race is still taken off before mem.
	bool ok = false;
	pthread_mutex_lock(&gadget_lock);
	if (adb_switched_on() && !suspend_in_progress && !dac_owns_usb && !usbaudio_active() &&
		!adb_gadget_bound()) {
		gadget_unbind_locked(); // the card reader lets go
		FILE *f = fopen(ADB_GADGET "/UDC", "w");
		if (f) {
			ok = fprintf(f, "%s\n", udc) >= 0;
			if (fclose(f) != 0) {
				ok = false;
			}
		}
	}
	pthread_mutex_unlock(&gadget_lock);

	// Once per outcome: adbd not yet ready is retried every poll.
	static int said = -1;
	if (said != (int)ok) {
		said = ok;
		fprintf(stderr, "usb: ADB is on but its gadget was not bound; %s\n",
				ok ? "handed it the controller" : "adbd is not ready yet, retrying");
	}
}

// ---------------------------------------------------------------------------
// Around a system suspend
//
// The reader is normally down whenever there is no cable, so a suspend usually
// finds nothing bound. A gadget left on the controller still makes this SoC
// answer with a reset instead of a resume, so prepare() takes every gadget off
// unconditionally and the watcher stands down until wake. resume() only clears
// that stand-down: the reader is rebuilt by the watcher on the next cable,
// never here.
// ---------------------------------------------------------------------------

// The cable, as the USB subsystem itself sees it -- not as the battery poll's
// five-second-old "charging" reading sees it. The suspend rule needs the
// truthful answer: a PC that enumerates the card must never have the gadget
// pulled out from under it.
bool usb_vbus_present(void) { return vbus_present(); }

void usb_suspend_prepare(void) {
	if (!sd_device[0]) {
		return; // USB mode was never started (host build, or no card)
	}
	// Stand every owner down before waiting for an export that may be in
	// flight: storage_export() checks this at its checkpoints and aborts.
	suspend_in_progress = true;

	// An export holds storage_lock across the unmount and the medium insertion.
	// Wait for it, then give the card back if the host still has it. A card left
	// exported across mem comes back with the player unable to mount it.
	pthread_mutex_lock(&storage_lock);
	bool suppressed = suppress_popups;
	suppress_popups = true;
	if (storage_active) {
		fprintf(stderr, "usb: suspend; taking the card back from the host\n");
		// Mount the card but leave the databases, log and caches closed: this
		// runs after the suspend's sync(), and opening them again would leave
		// handles on a card that is about to lose power. The wake path reopens
		// them (services_closed_for_suspend).
		storage_restore(false);
		services_closed_for_suspend = true;
	}
	suppress_popups = suppressed;
	pthread_mutex_unlock(&storage_lock);

	// Every gadget, not just ours: with ADB on it is adbd's gadget that holds
	// the controller. Hold the same lock the builder uses, or the watcher can
	// rebind the reader from under the suspend.
	pthread_mutex_lock(&gadget_lock);
	int rc = system("for u in /sys/kernel/config/usb_gadget/*/UDC; do echo '' > $u 2>/dev/null; done");
	(void)rc;
	pthread_mutex_unlock(&gadget_lock);
	// Taking an export back remounted the card after the power path's own sync().
	// Flush the mount's metadata before the card loses power.
	sync();
	fprintf(stderr, "usb: gadgets unbound for suspend\n");
}

void usb_resume_restore(void) {
	if (!sd_device[0]) {
		return;
	}
	// Neither the reader nor ADB's gadget is pre-positioned until the first
	// cable. The controller just came out of mem with no host; binding before
	// one arrives is what leaves dwc2's setup queue armed and stalls the next
	// attach.
	resume_wait_cable = true;
	// The first attach after a suspend runs the full builder. The cheap UDC
	// toggle is enough for a normal replug, but mem is the case where the
	// configfs/driver state has surprised us before.
	gadget_needs_build = true;

	if (services_closed_for_suspend) {
		// usb_suspend_prepare() had to take an exported card back. The mount is
		// already restored; this reopens the databases, log and caches that the
		// export closed. Hold storage_lock: storage_reopen_services() clears
		// storage_active, and that write must not land in the middle of an
		// export that started on the other side of the wake.
		services_closed_for_suspend = false;
		pthread_mutex_lock(&storage_lock);
		storage_reopen_services();
		pthread_mutex_unlock(&storage_lock);
	}

	// Only now may the watcher act on a cable. It stands down while the flag is
	// up, so nothing can bind or export during the reopen above.
	suspend_in_progress = false;
	fprintf(stderr, "usb: awake; no gadget until a cable arrives\n");
}


// True while something is mounted on sd_mount (paths resolved, because the
// firmware reaches the card through symlinks and /proc/mounts lists the
// canonical one).
static bool mount_still_up(void) {
	// realpath() takes a buffer of PATH_MAX: given less it may write past it.
	char wanted[PATH_MAX];
	if (!realpath(sd_mount, wanted)) {
		return false;
	}

	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return false;
	}

	char device[256], point[256];
	bool found = false;
	while (fscanf(f, "%255s %255s %*[^\n]", device, point) == 2) {
		if (strcmp(point, wanted) == 0) {
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}

// True when the host has ejected the medium: on a removable lun the "safely
// remove" on the PC closes the gadget's backing file, so lun.0/file reads
// empty. That is the signal to take the card back -- otherwise the player
// sits saying "no card" until the cable is physically pulled.
static bool host_ejected(void) {
	const char *lun_file = STORAGE_GADGET "/functions/mass_storage.0/lun.0/file";

	char *content = read_file_content(lun_file);
	if (!content) {
		return false; // node unreadable: assume still exported, do nothing rash
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool ejected = content[0] == '\0';
	free(content);
	return ejected;
}

// The medium as configfs reports it. Writing the path and getting no error is
// not enough: the function accepts the write even when the host or the LUN is
// in a state that leaves the file empty, and the watcher would then believe it
// had exported a card the PC can never see.
static bool storage_medium_present(const char *lun_file) {
	char *content = read_file_content(lun_file);
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool present = strcmp(content, sd_device) == 0;
	free(content);
	return present;
}

static bool storage_set_medium(const char *lun_file) {
	FILE *f = fopen(lun_file, "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", sd_device) >= 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	return ok && storage_medium_present(lun_file);
}

// Puts the medium in while the reader is off the controller, then binds it.
// This is the sequence that makes a host read the LUN again: writing the file
// into an already-enumerated reader leaves a PC or phone showing the empty
// card it cached at attach, and a vendor soft-disconnect is a no-op on some
// kernels even when the attribute exists.
//
// Under no lock on entry. Every step is verified, and a failure leaves the
// gadget either bound without a medium (the harmless empty reader) or off the
// controller for the next attempt.
static bool storage_present_medium(const char *lun_file) {
	for (int attempt = 0; attempt < 3; attempt++) {
		if (!reader_may_bind() || apps_own_usb) {
			return false;
		}
		// The role can have been re-arbitrated while the card was unmounted.
		typec_force_sink();

		pthread_mutex_lock(&gadget_lock);
		bool unbound = gadget_unbind_locked();
		usleep(200 * 1000);
		bool inserted = unbound && storage_set_medium(lun_file);
		bool bound = inserted && gadget_rebind_locked();
		if (!bound) {
			// The function may be missing or the controller was handed back a
			// moment ago. Rebuild, then take it off the controller again so the
			// medium goes in before the final bind, not after it.
			ensure_storage_gadget_locked();
			if (reader_may_bind() && !apps_own_usb && gadget_unbind_locked()) {
				inserted = storage_set_medium(lun_file);
				bound = inserted && gadget_rebind_locked();
			}
		}
		bool present = bound && storage_medium_present(lun_file);
		pthread_mutex_unlock(&gadget_lock);

		if (present) {
			fprintf(stderr, "usb: medium inserted (%s) and the reader re-enumerated\n", sd_device);
			return true;
		}
		fprintf(stderr, "usb: could not present %s (attempt %d of 3)\n", sd_device, attempt + 1);
		usleep(250 * 1000);
	}
	return false;
}

// The services that live on the card, reopened after either a finished export
// or an aborted one. Separated from storage_restore() because an export that
// never got as far as unmounting must not unmount and remount a perfectly good
// filesystem just to reopen them.
static void storage_reopen_services(void) {
	char db_path[512];
	snprintf(db_path, sizeof(db_path), "%s/.local/library.db", sd_mount);
	library_open(db_path);
	snprintf(db_path, sizeof(db_path), "%s/.local/audiobooks.db", sd_mount);
	audiobookdb_open(db_path);
	logging_resume_after_usb();
	btlog_card_attach(sd_mount);

	cover_set_cache_dir(sd_mount);

	storage_active = false;
	library_card_returned(sd_mount);
}

// Undo the half of an export that already ran: the card was taken away from
// the player and the services on it were closed, but the host never got a
// usable medium. If it was never unmounted, only the services have to come
// back; otherwise storage_restore() does the remount as well. The "available
// again" popup is not wanted for an attempt that never worked.
static void storage_abort_export(void) {
	bool suppressed = suppress_popups;
	suppress_popups = true;
	if (mount_still_up()) {
		fprintf(stderr, "usb: export aborted; the card stayed mounted\n");
		storage_reopen_services();
	} else {
		storage_restore(true);
	}
	suppress_popups = suppressed;
	export_retry_at = time(NULL) + 10;
}

// Hands the card to the host. Ordering is what the stock player does: stop
// touching the filesystem, unmount, then export the raw device.
static void storage_export_locked(void) {
	if (apps_own_usb) {
		// The Apps feature has the card. The watcher checks this before
		// calling, but a check here closes the window between its poll and
		// this function taking the lock.
		fprintf(stderr, "usb: apps are using the card; not exporting\n");
		return;
	}
	if (suspend_in_progress) {
		fprintf(stderr, "usb: suspend is starting; not exporting\n");
		return;
	}
	if (adb_owns_controller()) {
		// ADB was switched on between the watcher's check and this call. Do not
		// unmount the card or touch a gadget: the next poll sees ADB and waits,
		// and confirming USB storage is what turns ADB off (see apphost).
		fprintf(stderr, "usb: ADB is on; not exporting\n");
		return;
	}

	fprintf(stderr, "usb: host attached, exporting %s\n", sd_device);

	// Said first, not last.
	//
	// The card supervisor stands down while this flag is up (see
	// storage_recheck_card): a card handed to a PC on purpose must not be taken
	// back from under it. Everything below -- stopping playback, closing the
	// databases, up to three unmount attempts a second apart, the
	// re-enumeration -- takes longer than the supervisor's four-second poll, and
	// all the supervisor needs to see is the moment after the unmount: no mount,
	// nobody claiming the card, so it mounts the card again. From then on the PC
	// and the player write to the same medium, and the player reads a stale
	// snapshot of the filesystem.
	storage_active = true;

	// Playback reads from the card; the library and the audiobook index hold
	// open databases on it; the log usually *is* on it -- and an open
	// descriptor makes the unmount fail. Release everything before trying.
	//
	// Where the track was, first of all: the stop below is what the next press
	// of play would otherwise read as "the track ended, start it again".
	device_state_note_storage_gone();
	audio_stop();
	library_close();
	audiobookdb_close();
	logging_suspend_for_usb();
	btlog_card_release();

	// The thumbnail cache is another open file on the card, and missing it is
	// what made the export fail to unmount it. It goes back on the card in
	// storage_restore().
	cover_set_cache_dir(NULL); // the thumbnail cache moves to /tmp

	if (sysserver_available()) {
		sysserver_umount(sd_mount);
	} else {
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "umount %s", sd_mount);
		int urc = system(cmd);
		(void)urc;
	}

	// Handing the raw device to a PC while the card is still mounted here would
	// corrupt the filesystem. If the first unmount did not take (a straggler
	// still holding a file), escalate: plain, forced, then lazy.
	//
	// The loop also gives up if DAC mode takes the controller meanwhile:
	// carrying on would insert the medium into what is by then the audio
	// gadget, and the card would appear on the PC in DAC mode.
	for (int attempt = 0; attempt < 3 && mount_still_up() && !dac_owns_usb && !suspend_in_progress &&
		 !apps_own_usb; attempt++) {
		const char *flags = (attempt == 0) ? "" : (attempt == 1) ? "-f " : "-l ";
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "umount %s%s", flags, sd_mount);
		fprintf(stderr, "usb: card still mounted, retrying: %s\n", cmd);
		int urc = system(cmd);
		(void)urc;
		sleep(1);
	}
	if (dac_owns_usb || usbaudio_active() || adb_owns_controller() || suspend_in_progress || apps_own_usb) {
		fprintf(stderr, "usb: another owner took the controller; export abandoned\n");
		storage_abort_export(); // the card and the databases have to come back
		return;
	}
	if (mount_still_up()) {
		// Exporting a card the process still has mounted hands the host a
		// filesystem whose metadata is already cached here. One of the two
		// writers will lose; usually it is the user's data. Leave the card
		// with the player and retry after the holder has gone.
		fprintf(stderr, "usb: could not unmount %s; leaving the card with the player\n", sd_mount);
		storage_abort_export();
		return;
	}

	// ADB is off (checked above), but its stopped gadget can still hold the
	// controller for the moment before the stop thread reclaims it. Let it go
	// so the reader can bind; a second gadget can never attach.
	if (adb_gadget_bound()) {
		fprintf(stderr, "usb: releasing the stopped ADB gadget\n");
		adb_gadget_unbind();
	}

	// The reader is the only gadget now. ensure_storage_gadget() returns
	// immediately when it is already built and bound, and after an ADB stop it
	// is what puts it back. Without this, the medium write below races the stop
	// thread and fails against a gadget that is not there yet -- the card then
	// stays invisible until ADB is toggled again.
	ensure_storage_gadget();

	if (dac_owns_usb || usbaudio_active() || adb_owns_controller() || suspend_in_progress || apps_own_usb) {
		fprintf(stderr, "usb: another owner took the controller before the medium went in\n");
		storage_abort_export();
		return;
	}

	const char *lun = STORAGE_GADGET "/functions/mass_storage.0/lun.0/file";
	if (!storage_present_medium(lun)) {
		fprintf(stderr, "usb: could not present the card after re-enumeration\n");
		storage_abort_export();
		return;
	}

	// What the controller thinks now, for the log: "CONFIGURED" means the PC
	// accepted the device.
	int lrc = system("for u in /sys/class/udc/*/state; do echo \"usb: udc $u: $(cat $u)\" >&2; done");
	(void)lrc;

	export_retry_at = 0;
	if (!suppress_popups) {
		gui_post(popup_connected_cb, NULL);
	}
}

// Holds the card supervisor's lock across the whole export. The supervisor
// checks usb_storage_active() before taking that lock, so holding it here is
// what closes the window between its check and its mount: once the export owns
// the lock, the supervisor cannot mount again until the export has finished or
// aborted (and by then the flag tells it to stay away).
static void storage_export(void) {
	storage_card_lock();
	storage_export_locked();
	storage_card_unlock();
}

// Takes the card back from the host and puts everything the export tore down
// back in place.
static void storage_restore(bool reopen_services) {
	fprintf(stderr, "usb: host detached, reclaiming %s\n", sd_device);

	// Take the medium out; the gadget stays bound (the PC now sees a card
	// reader with no card, which is exactly the truth). The watcher takes the
	// gadget off the controller when the cable goes.
	int rc = system("echo '' > " STORAGE_GADGET "/functions/mass_storage.0/lun.0/file");
	fprintf(stderr, "usb: medium removed, exit code %d\n", rc);

	// Whatever is mounted now is a stale view of the card and has to go.
	//
	// The kernel cached that filesystem's metadata before handing the raw
	// device to the host; everything the PC wrote since is invisible through
	// it. Usually there is nothing left mounted, because the export unmounted
	// it -- but not always, since the export gives up after three attempts and
	// exports anyway. The loop below would then find the mount point "already
	// up" and leave it alone, and the player would carry on reading a directory
	// that no longer describes the card.
	//
	// So: drop it first, then mount. Unmounting a mount point that reads fine
	// costs a syscall; reading a card through a stale one costs the user their
	// firmware update.
	if (mount_still_up()) {
		fprintf(stderr, "usb: dropping the pre-export mount before reading the card again\n");
		if (sysserver_available()) {
			sysserver_umount(sd_mount);
		}
		if (mount_still_up()) {
			char cmd[2 * sizeof(sd_mount) + 64];
			snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null || umount -l %s", sd_mount, sd_mount);
			int urc = system(cmd);
			(void)urc;
		}
	}

	// Mount it back, and check rather than hope: a remount that fails silently
	// leaves the player with no card until a reboot. Try the daemon, then the
	// syscall, then busybox, until the mount point actually reads.
	for (int attempt = 0; attempt < 6 && !mount_still_up(); attempt++) {
		if (attempt) {
			sleep(1);
		}
		if (sysserver_available() && sysserver_mount(sd_device, sd_mount) == 0) {
			continue;
		}
		// The same order as at startup (mount_sd_directly in system.c): NTFS
		// last, and ntfs-3g before the kernel's ntfs module, which mounts
		// read-only and would leave the card browsable and nothing else after
		// every USB session. The types are named rather than guessed: busybox
		// guessing tries ntfs like any other type. `force,remove_hiberfile` is
		// what lets ntfs-3g mount a card the PC was unplugged from without an
		// eject, or that Windows fast startup left hibernated -- see the
		// matching options in mount_ntfs_rw().
		char remount[1600];
		snprintf(remount, sizeof(remount),
				 "mount -t exfat,vfat,ext4,ext3,ext2,ntfs3 %s %s 2>/dev/null || "
				 "ntfs-3g %s %s -o force,remove_hiberfile,big_writes,noatime 2>/dev/null || "
				 "mount -t ntfs %s %s",
				 sd_device, sd_mount, sd_device, sd_mount, sd_device, sd_mount);
		int mrc = system(remount);
		(void)mrc;
	}
	fprintf(stderr, "usb: card remounted on %s: %s\n", sd_mount, mount_still_up() ? "yes" : "NO");

	// The databases and the log live on the card that just came back. Both
	// cache directories fall back to /tmp on their own if the remount did not
	// take, so a failed remount leaves the player usable rather than stuck on
	// a handle to nowhere. The callback also starts the post-USB scan.
	if (reopen_services) {
		storage_reopen_services();
		if (!suppress_popups) {
			gui_post(popup_disconnected_cb, NULL);
		}
	} else {
		// The suspend path reopens the services after the wake; opening the
		// databases now would leave handles on a card that is about to lose
		// power.
		storage_active = false;
	}
}

// Takes the card back from the host if it was handed over, from any thread.
static void take_card_back(const char *why) {
	pthread_mutex_lock(&storage_lock);
	if (storage_active) {
		fprintf(stderr, "usb: %s; taking the card back from the host\n", why);
		storage_restore(true);
	}
	pthread_mutex_unlock(&storage_lock);
}

// The watcher's own export and restore, under the same lock.
static void locked_export(void) {
	pthread_mutex_lock(&storage_lock);
	storage_export();
	pthread_mutex_unlock(&storage_lock);
}

static void locked_restore(void) {
	pthread_mutex_lock(&storage_lock);
	if (storage_active) {
		storage_restore(true);
	}
	pthread_mutex_unlock(&storage_lock);
}

// Puts the medium back after the reader fell off, under the same lock the
// export uses. Without it a concurrent take_card_back() (apps, DAC, ADB) could
// restore the card while this thread is handing the raw device to the host.
static bool locked_present_medium(void) {
	pthread_mutex_lock(&storage_lock);
	bool ok = false;
	if (storage_active && !apps_own_usb && reader_may_bind()) {
		ok = storage_present_medium(STORAGE_GADGET "/functions/mass_storage.0/lun.0/file");
	}
	pthread_mutex_unlock(&storage_lock);
	return ok;
}

// The watcher is the only writer of the reader's lifecycle. Every other thread
// publishes intent in the flags above; this loop turns that intent into the
// controller's actual state.
//
// The rule is deliberately blunt: the reader exists only while a cable is
// present and no other owner has the controller. There is no reader in standby,
// no reader left across an unplug, and no "rebind later" flag. Every attach
// starts with a clean UDC toggle, which clears the stale dwc2 setup queue that
// used to make the next host sit at enqueue_setup -EAGAIN.
static void *usb_thread(void *arg) {
	(void)arg;
	thread_be_background("usb watcher");

	// True between a cable appearing and disappearing.
	bool session = false;
	uint32_t session_ms = 0;
	bool enum_retry_done = false;

	// The PC asked for the medium to be ejected. The card is back with the
	// player; do not export again until this host goes away.
	bool await_unplug = false;

	// Once per session, so a declined prompt or a repeated "ADB is on" line
	// does not fill the log.
	bool said_apps = false;
	bool said_adb = false;
	bool was_configured = false;

	// Backoff for the screen-on pre-position when the reader cannot bind (no
	// UDC, configfs read-only, a foreign gadget). Without it a persistent
	// failure would fork the 2 KB builder shell once a second.
	uint32_t prepos_retry_ms = 0;

	for (;;) {
		// The power thread is between unbinding the gadget and writing mem.
		// Anything this loop did now could put a gadget back on the controller
		// and turn the suspend into a reset.
		if (suspend_in_progress) {
			sleep(1);
			continue;
		}

		// DAC mode builds its audio function in the same configfs tree. Nothing
		// in this file may write to it.
		if (dac_owns_usb) {
			sleep(1);
			continue;
		}

		// A USB DAC or headphones make this port the host. The controller is
		// not ours to bind a gadget to.
		if (usbaudio_active()) {
			sleep(1);
			continue;
		}

		sleep(power_screen_is_on() ? POLL_SECONDS : POLL_SECONDS_STANDBY);

		// The sleep is long enough for DAC mode, a headset or the suspend to
		// take the controller.
		if (suspend_in_progress || dac_owns_usb || usbaudio_active()) {
			continue;
		}

		bool configured = udc_configured();
		bool vbus = vbus_present() || configured;

		// After mem the reader is not pre-positioned until a cable shows up. The
		// flag covers that case only; ADB is gated by VBUS below.
		if (vbus) {
			resume_wait_cable = false;
			// ADB's gadget needs the one controller, and it is useless without a
			// cable. Binding it here means the host is already on the wire after a
			// mem wake or a charger-to-PC swap, never before it.
			adb_keep_controller();
		}

		// The enumerated host went away. VBUS can stay high after an unplug (a
		// charger, or a phone that keeps the port powered), so this edge is
		// tracked separately from the cable.
		if (was_configured && !configured) {
			await_unplug = false;
			said_apps = false;
			said_adb = false;
			// A host can come and go without VBUS dropping (a phone that keeps
			// the port powered). Give the next enumeration its own watchdog
			// window instead of inheriting the previous attach's one shot.
			enum_retry_done = false;
			session_ms = monotonic_ms();
		}
		was_configured = configured;

		if (!vbus) {
			// Cable gone (or never reported). Give the card back first.
			if (storage_active) {
				locked_restore();
			}

			// ADB's gadget is useless without a cable, and leaving it bound across
			// an unplug is what arms dwc2's setup queue for the next host. Unbind
			// it here; it is bound again on the next cable, with the host present.
			if (adb_gadget_bound()) {
				adb_gadget_unbind();
				fprintf(stderr, "usb: cable gone; ADB gadget unbound\n");
			}

			if (adb_owns_controller()) {
				// ADB owns the controller: the reader must not be on the bus.
				if (storage_gadget_bound() && gadget_unbind()) {
					fprintf(stderr, "usb: ADB takes the controller; reader unbound\n");
				}
			} else if (power_screen_is_on() && !resume_wait_cable) {
				// Keep an empty reader on the bus while the screen is on. Some
				// hosts (phones, in particular) act as the data host while the
				// PMIC's "online" node reads 0; with no bound gadget they can
				// never enumerate us. A real cable still forces a clean rebuild
				// when VBUS appears, so this is not the old stale-reader case.
				uint32_t now = monotonic_ms();
				if (!storage_gadget_bound() && (!prepos_retry_ms || (int32_t)(now - prepos_retry_ms) >= 0)) {
					reader_reattach();
					prepos_retry_ms = storage_gadget_bound() ? 0 : now + 10000;
				}
			} else if (storage_gadget_bound() && gadget_unbind()) {
				// Panel dark and no cable: nothing can enumerate it, and a bound
				// gadget keeps the PHY and OTG block clocked all night.
				fprintf(stderr, "usb: cable gone; reader unbound\n");
			}

			session = false;
			enum_retry_done = false;
			await_unplug = false;
			said_apps = false;
			said_adb = false;
			export_retry_at = 0;
			continue;
		}

		if (!session) {
			// A new cable. A reader left bound across an unplug can have dwc2's
			// setup queue armed for the old host; the failure shows up as the
			// host never reaching "configured". Start with a clean UDC toggle
			// unless the host has already enumerated the reader we had on the
			// bus, in which case toggling would only interrupt it.
			session = true;
			session_ms = monotonic_ms();
			enum_retry_done = false;
			await_unplug = false;
			said_apps = false;
			said_adb = false;
			export_retry_at = 0;

			if (adb_owns_controller()) {
				fprintf(stderr, "usb: cable in; ADB owns the controller\n");
				continue;
			}

			if (configured && storage_gadget_bound()) {
				fprintf(stderr, "usb: host enumerated the ready reader\n");
			} else {
				fprintf(stderr, "usb: cable in; building the reader\n");
				reader_reattach();
				continue; // one poll for the host to enumerate
			}
		}

		if (adb_owns_controller()) {
			// One controller, one gadget. Our reader must not be on it. If the
			// Apps feature holds the card, a host that is here for storage still
			// gets the way out: confirming the prompt turns ADB off first.
			if (storage_active) {
				locked_restore();
			}
			if (storage_gadget_bound() && gadget_unbind()) {
				fprintf(stderr, "usb: ADB takes the controller; reader unbound\n");
			}
			if (configured && apps_own_usb && !said_apps && !apphost_mass_storage_closing()) {
				fprintf(stderr, "usb: host attached while apps are in use; asking\n");
				if (gui_post(apphost_prompt_mass_storage, NULL)) {
					said_apps = true;
				}
			}
			if (!said_adb) {
				fprintf(stderr, "usb: ADB is on; the card stays mounted (no mass storage)\n");
				said_adb = true;
			}
			continue;
		}
		said_adb = false;

		// The reader is supposed to be on the controller. If it is not, a
		// failed bind or a gadget torn down elsewhere left it off; rebuild it
		// rather than waiting for a state change that will never come.
		if (!storage_gadget_bound()) {
			if (storage_active) {
				bool ok = locked_present_medium();
				fprintf(stderr, "usb: exported reader fell off the controller; %s\n",
						ok ? "rebound with the medium" : "could not be restored");
				if (!ok) {
					locked_restore();
					export_retry_at = time(NULL) + 10;
				}
			} else {
				fprintf(stderr, "usb: reader was not on the controller; rebuilding\n");
				reader_reattach();
			}
			continue; // one poll for the host to enumerate
		}

		if (storage_active) {
			if (host_ejected()) {
				fprintf(stderr, "usb: host ejected the medium, reclaiming the card\n");
				locked_restore();
				await_unplug = true;
			}
			continue;
		}

		if (await_unplug) {
			// The host asked for the medium to go; do not put it back under it.
			// A real detach clears this above, when the UDC leaves configured.
			continue;
		}

		if (!configured) {
			// VBUS without enumeration is either a charger or a host that the
			// stale setup queue never reached. Try one clean UDC toggle, with the
			// host already on the cable. A charger never enumerates, so the
			// one-shot is what keeps this from becoming a toggle loop.
			if (!enum_retry_done && (uint32_t)(monotonic_ms() - session_ms) >= ENUM_REBUILD_MS) {
				enum_retry_done = true;
				fprintf(stderr, "usb: host on the cable but not enumerated; rebuilding the reader\n");
				reader_reattach();
			}
			continue;
		}

		// A configured host that is here for the card. The apps guard keeps the
		// medium out and asks; otherwise the card goes to the host.
		if (apps_own_usb) {
			if (!said_apps && !apphost_mass_storage_closing()) {
				fprintf(stderr, "usb: host attached while apps are in use; asking\n");
				if (gui_post(apphost_prompt_mass_storage, NULL)) {
					said_apps = true;
				}
			}
			continue;
		}
		said_apps = false;

		// A previous export failed while the holder of the card was still on
		// it. Do not unmount/remount it once a second; give it a moment. Asked
		// after the prompt checks so a plug-in is never ignored just because
		// the last export failed.
		if (export_retry_at && time(NULL) < export_retry_at) {
			continue;
		}

		locked_export();
	}

	return NULL;
}

void usb_start(const char *device, const char *mount_point) {
	if (!device || !device[0] || !mount_point || !mount_point[0]) {
		fprintf(stderr, "usb: no card device/mount known; USB mode disabled\n");
		return; // host build, or no card: nothing to export
	}

	bool any_vbus = false;
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		bool have = access(VBUS_CANDIDATES[i], R_OK) == 0;
		fprintf(stderr, "usb: vbus node %s: %s\n", VBUS_CANDIDATES[i], have ? "present" : "missing");
		any_vbus |= have;
	}
	if (!any_vbus) {
		fprintf(stderr, "usb: no VBUS node found; USB mode disabled\n");
		return;
	}
	if (access("/sys/kernel/config", F_OK) != 0) {
		fprintf(stderr, "usb: no configfs on this kernel; USB mode disabled\n");
		return;
	}

	// The port is a peripheral from here on, whatever is at the other end of
	// the cable. The stock player re-applies its own saved choice at every boot
	// for the same reason: the role is not remembered across one.
	typec_force_sink();

	snprintf(sd_device, sizeof(sd_device), "%s", device);
	snprintf(sd_mount, sizeof(sd_mount), "%s", mount_point);

	// Nothing is bound here. The watcher pre-positions an empty reader while the
	// screen is on and rebuilds it on the first cable; a device that boots in
	// standby pays nothing for USB until it is needed.
	pthread_t thread;
	if (pthread_create(&thread, NULL, usb_thread, NULL) == 0) {
		usb_started = true;
		pthread_detach(thread);
		fprintf(stderr, "usb: watching for a cable (exporting %s)\n", sd_device);
	} else {
		sd_device[0] = '\0';
		sd_mount[0] = '\0';
		fprintf(stderr, "usb: could not start the watcher; USB mode disabled\n");
	}
}
