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
// and binds it to the device's one USB controller. That matters here: with
// ADB enabled the mass-storage script's own gadget can never bind (the
// controller is busy), which on the stock firmware is resolved by making the
// two exclusive (adbon/adboff swap them). This player does better: when the
// ADB gadget owns the controller, the mass-storage function is added into
// *that* gadget (a composite device, the way Android phones expose adb+MTP
// together), so the PC gets the disk and the adb session both.
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

// Counts the times the controller has changed hands (DAC mode, ADB). The
// watcher forgets what it knew about the cable when it moves: an eject done by
// the host before the handover says nothing about the device after it, and a
// card taken back by someone else is no longer exported.
static unsigned owner_serial;

static void owner_changed(void) { __atomic_add_fetch(&owner_serial, 1, __ATOMIC_SEQ_CST); }

// Which way the gadget was exported, so the restore tears down the same one.
static enum { EXPORT_NONE, EXPORT_OWN_GADGET, EXPORT_ADB_COMPOSITE } export_mode;

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

// Set while the Apps feature is using the card. The watcher keeps running its
// cable and parking housekeeping but never exports the medium; an export in
// flight when this goes up is taken back by usb_set_apps_mode(). The empty
// reader stays on the bus: that is what lets the watcher recognise a PC (as
// opposed to a charger) and offer to close the app and share the card.
static volatile bool apps_own_usb;

// After mem the reader is not bound until a cable appears; binding before the
// host does stalls dwc2's setup queue (see usb_resume_restore()). Set on wake,
// cleared by the watcher's first-cable rebuild. Every transition that would
// otherwise bind the reader has to leave it alone while this is set.
static volatile bool rebind_on_cable;

// Set across the suspend hand-off, from the unbind in usb_suspend_prepare() to
// usb_resume_restore(). The watcher stands down meanwhile: it sleeps for up to
// a poll interval, and a rebind landing between the unbind and the write to
// /sys/power/state leaves a gadget on the controller, which is what makes this
// SoC reset instead of suspending.
static volatile bool suspend_in_progress;

// Entering and leaving DAC mode moves the card back and forth exactly the way
// a cable being plugged in does, which would announce "Memoria di nuovo
// disponibile" or "Collegato al PC" for something the user did not do and
// cannot see. The popups are silenced for the duration of the transition.
static volatile bool suppress_popups;
static void ensure_storage_gadget(void);
static void ensure_storage_gadget_locked(void);
static void gadget_unbind(void);
static bool gadget_rebind(void);
static bool first_udc(char *out, size_t out_size);
static void storage_restore(void);
static void take_card_back(const char *why);

void usb_set_dac_mode(bool on) {
	// The flag goes up before anything else: setting it afterwards leaves a
	// window in which the watcher thread can wake, see a cable and re-export
	// the card this call is in the middle of taking back.
	suppress_popups = true;
	if (on) {
		dac_owns_usb = true;
		// Take the card back from the computer. In DAC mode the player is a
		// sound card and nothing else: handing over the microSD at the same
		// time is not a bonus, it is the card being pulled out from under the
		// player while it works.
		//
		// Only when it was handed over. A restore with nothing exported still
		// unmounts and remounts the card the player is reading from.
		take_card_back("DAC mode takes the controller");
	}
	dac_owns_usb = on;
	owner_changed();
	fprintf(stderr, "usb: controller %s\n", on ? "handed to DAC mode" : "back from DAC mode");
	if (!on) {
		// Entering DAC mode took the controller from ADB's gadget as well. With
		// ADB on it goes back to that one (adb_keep_controller(), at the next
		// poll): binding the card reader first would only be undone a second
		// later.
		if (adb_owns_controller()) {
			fprintf(stderr, "usb: ADB is on; its gadget gets the controller back\n");
		} else if (!rebind_on_cable) {
			// After mem the reader waits for the first cable; the watcher does the
			// full UDC toggle there. Binding it here is the stall all over again.
			ensure_storage_gadget();
		}
	}
	suppress_popups = false;
}

bool usb_dac_mode(void) {
	return dac_owns_usb;
}

void usb_set_apps_mode(bool on) {
	if (apps_own_usb == on) {
		return;
	}

	// The flag goes up before the export can be started or the card taken
	// back: the watcher thread must see it first.
	apps_own_usb = on;
	owner_changed();

	if (!usb_started) {
		return;
	}

	suppress_popups = true;
	if (on) {
		// An app runs from a file on the card, so a card already handed to a
		// host has to come back. Only when it was exported: a restore with
		// nothing exported still unmounts and remounts the card.
		take_card_back("apps are using the card");
	} else if (!dac_owns_usb && !adb_owns_controller() && !rebind_on_cable) {
		// Put the empty-medium reader back on the controller; the watcher
		// exports on its next poll if a host is still there. With ADB on the
		// controller is its gadget's, and mass storage is a separate choice
		// that turns ADB off (see apphost). While rebind_on_cable is set the
		// reader is deliberately off the bus: opening and closing an app after
		// mem must not bind it a poll before the cable arrives.
		ensure_storage_gadget();
	}
	suppress_popups = false;

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
// It is bound with an empty medium rather than only while exporting because a
// bound gadget is what lets the controller's state tell a PC from a wall
// charger: a PC enumerates the medium-less card reader and the UDC state reads
// CONFIGURED, a charger only supplies power and never does. The stock player
// makes the same distinction the same way, with its UAC gadget. Exporting is
// then just writing the block device into lun.0/file (the medium clicks in);
// un-exporting writes it empty again. The gadget itself never unbinds.
static void ensure_storage_gadget_locked(void) {
	// apps_own_usb is deliberately not checked here. The guard keeps the
	// *medium* out of the host (storage_export() enforces that); it must not
	// keep the empty card reader off the bus. A bound reader is how the watcher
	// recognises that a PC -- not a charger -- is attached, which is the only
	// way the "close the apps?" prompt can ever be shown. Refusing to bind
	// while an app was alive is how a second plug-in used to do nothing at all.
	if (dac_owns_usb || usbaudio_active()) {
		return;
	}

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
	}
	// The shell is slow; the audio side can claim the controller while it
	// runs. Do not bind the reader over the gadget it just built.
	if (dac_owns_usb || usbaudio_active()) {
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
static void gadget_unbind(void) {
	pthread_mutex_lock(&gadget_lock);
	bool ok = gadget_unbind_locked();
	pthread_mutex_unlock(&gadget_lock);
	if (!ok) {
		fprintf(stderr, "usb: could not take the storage gadget off the controller\n");
	}
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
	take_card_back("ADB is starting");
	owner_changed();
	gadget_unbind();
}

// Called after ADB shuts down. Its stop script also unmounts the whole
// configfs, so the gadget is rebuilt from nothing.
//
// A card that was riding on ADB's gadget comes back first; the watcher then
// hands it to the host again through the card reader, if a host is there.
void usb_gadget_reclaim_from_adb(void) {
	take_card_back("ADB stopped");
	// The composite restore binds ADB's gadget again, and with adbd gone that
	// gadget has nothing left to carry but still holds the controller.
	if (!adb_is_running()) {
		FILE *f = fopen(ADB_GADGET "/UDC", "w");
		if (f) {
			fputs("\n", f);
			fclose(f);
		}
	}
	owner_changed();
	if (!rebind_on_cable) {
		ensure_storage_gadget();
	} else {
		// Still waiting for the first cable after mem; the watcher does the full
		// UDC toggle then. Binding here would put the reader back before the
		// host and leave dwc2's setup queue stalled again.
		fprintf(stderr, "usb: ADB stopped; the reader waits for the first cable after resume\n");
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
	if (rebind_on_cable) {
		// After mem nothing is bound to the controller until a cable turns up;
		// the watcher's first-cable path clears the flag, and the next poll binds
		// ADB's gadget with the host already attached.
		return;
	}
	if (!adb_is_running()) {
		return;
	}

	char udc[NAME_MAX + 1];
	if (!first_udc(udc, sizeof(udc))) {
		return;
	}
	gadget_unbind(); // the card reader lets go; failing when it holds nothing is fine

	bool ok = false;
	FILE *f = fopen(ADB_GADGET "/UDC", "w");
	if (f) {
		ok = fprintf(f, "%s\n", udc) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
	}

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
// The storage gadget is deliberately kept bound to the USB controller at all
// times (see above: a bound gadget is what lets the controller tell a PC from
// a wall charger). That is fine while the board is awake and fatal when it
// tries to sleep: suspending with a live gadget on the controller makes the
// device answer with a reset instead of a resume, and the player starts from
// scratch.
//
// So the controller is handed nothing at all before the SoC goes down, and the
// gadget is rebuilt on the way back. The stock player does not need this: it
// only builds a gadget while a host is actually attached.
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
	// Stand the watcher down first: setting this after the unbind leaves the
	// window this flag exists to close.
	suspend_in_progress = true;
	// Every gadget, not just ours: with ADB on it is adbd's gadget that holds
	// the controller. Hold the same lock the builder uses, or the watcher can
	// rebind the reader from under the suspend.
	pthread_mutex_lock(&gadget_lock);
	int rc = system("for u in /sys/kernel/config/usb_gadget/*/UDC; do echo '' > $u 2>/dev/null; done");
	(void)rc;
	pthread_mutex_unlock(&gadget_lock);
	fprintf(stderr, "usb: gadgets unbound for suspend\n");
}

void usb_resume_restore(void) {
	if (!sd_device[0]) {
		return;
	}

	// Do not bind the reader here. A gadget bound before a cable is present
	// comes back from mem with dwc2's setup queue stalled: the host attaches,
	// dwc2_hsotg_enqueue_setup fails with -EAGAIN every few seconds, and no
	// configuration is ever reached. The ADB toggle that used to fix it was
	// really a full gadget rebuild. Defer that rebuild to the first cable
	// instead (see the watcher), where the UDC is toggled with the host
	// already watching.
	rebind_on_cable = true;
	suspend_in_progress = false;

	if (adb_owns_controller()) {
		// ADB's gadget is bound by its own poll. If the user then chooses
		// storage, ADB lets go and usb_set_apps_mode() builds the reader.
		fprintf(stderr, "usb: ADB is on; the reader waits for the first cable after resume\n");
		return;
	}
	fprintf(stderr, "usb: reader waits for the first cable after resume\n");
}

// Adds the mass-storage function to the ADB gadget and rebinds it, giving the
// host a composite adb+storage device. Shell, because this is precisely the
// firmware's own scripting style and the sequence is standard configfs work:
// unbind, add function, link into the config, rebind. The lun settings match
// what the stock binary writes (ro=0 removable=1 nofua=1 cdrom=0). The adb
// connection drops for a moment at the rebind and comes back on its own.
static void composite_export(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
			 "cd %s && "
			 "echo '' > UDC; "
			 "echo '" USB_MANUFACTURER "' > strings/0x409/manufacturer 2>/dev/null; "
			 "echo '%s' > strings/0x409/product 2>/dev/null; "
			 "mkdir -p functions/mass_storage.0 && "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo %s > functions/mass_storage.0/lun.0/file; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1; "
			 "echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 ADB_GADGET, usb_product(), sd_device);
	fprintf(stderr, "usb: adding storage to the adb gadget\n");
	int rc = system(cmd);
	fprintf(stderr, "usb: composite export exit code %d\n", rc);
}

static void composite_restore(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
			 "cd %s && "
			 "echo '' > UDC; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 "rm -f configs/c.1/mass_storage.0; "
			 "rmdir functions/mass_storage.0; "
			 "echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 ADB_GADGET);
	int rc = system(cmd);
	fprintf(stderr, "usb: composite restore exit code %d\n", rc);
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
	const char *lun_file = (export_mode == EXPORT_ADB_COMPOSITE)
							   ? ADB_GADGET "/functions/mass_storage.0/lun.0/file"
							   : STORAGE_GADGET "/functions/mass_storage.0/lun.0/file";

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
		if (dac_owns_usb || usbaudio_active()) {
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
			if (!dac_owns_usb && !usbaudio_active() && gadget_unbind_locked()) {
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
		export_mode = EXPORT_NONE;
		storage_reopen_services();
	} else {
		if (export_mode == EXPORT_NONE) {
			export_mode = EXPORT_OWN_GADGET;
		}
		storage_restore();
	}
	suppress_popups = suppressed;
	export_retry_at = time(NULL) + 10;
}

// Hands the card to the host. Ordering is what the stock player does: stop
// touching the filesystem, unmount, then export the raw device.
static void storage_export(void) {
	if (apps_own_usb) {
		// The Apps feature has the card. The watcher checks this before
		// calling, but a check here closes the window between its poll and
		// this function taking the lock.
		fprintf(stderr, "usb: apps are using the card; not exporting\n");
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
	for (int attempt = 0; attempt < 3 && mount_still_up() && !dac_owns_usb; attempt++) {
		const char *flags = (attempt == 0) ? "" : (attempt == 1) ? "-f " : "-l ";
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "umount %s%s", flags, sd_mount);
		fprintf(stderr, "usb: card still mounted, retrying: %s\n", cmd);
		int urc = system(cmd);
		(void)urc;
		sleep(1);
	}
	if (dac_owns_usb || usbaudio_active()) {
		fprintf(stderr, "usb: the audio side took the controller; export abandoned\n");
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
		FILE *adb_udc = fopen(ADB_GADGET "/UDC", "w");
		if (adb_udc) {
			fputs("\n", adb_udc);
			fclose(adb_udc);
		}
		owner_changed();
	}

	// The reader is the only gadget now. ensure_storage_gadget() returns
	// immediately when it is already built and bound, and after an ADB stop it
	// is what puts it back. Without this, the medium write below races the stop
	// thread and fails against a gadget that is not there yet -- the card then
	// stays invisible until ADB is toggled again.
	ensure_storage_gadget();

	if (dac_owns_usb || usbaudio_active()) {
		fprintf(stderr, "usb: the audio side took the controller before the medium went in\n");
		storage_abort_export();
		return;
	}

	if (adb_switched_on() && adb_gadget_bound()) {
		// Kept for a controller whose ADB gadget is deliberately shared, but
		// the watcher never reaches here with ADB on: storage and ADB are
		// exclusive now.
		composite_export();
		export_mode = EXPORT_ADB_COMPOSITE;
		if (!storage_medium_present(ADB_GADGET "/functions/mass_storage.0/lun.0/file")) {
			fprintf(stderr, "usb: the composite medium did not appear; aborting the export\n");
			storage_abort_export();
			return;
		}
	} else {
		const char *lun = STORAGE_GADGET "/functions/mass_storage.0/lun.0/file";
		export_mode = EXPORT_OWN_GADGET;
		if (!storage_present_medium(lun)) {
			fprintf(stderr, "usb: could not present the card after re-enumeration\n");
			storage_abort_export();
			return;
		}
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

// Takes the card back from the host and puts everything the export tore down
// back in place.
static void storage_restore(void) {
	fprintf(stderr, "usb: host detached, reclaiming %s\n", sd_device);

	if (export_mode == EXPORT_ADB_COMPOSITE) {
		composite_restore();
	} else {
		// Take the medium out; the gadget stays bound (the PC now sees a card
		// reader with no card, which is exactly the truth).
		int rc = system("echo '' > " STORAGE_GADGET "/functions/mass_storage.0/lun.0/file");
		fprintf(stderr, "usb: medium removed, exit code %d\n", rc);
	}
	export_mode = EXPORT_NONE;

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
	storage_reopen_services();
	if (!suppress_popups) {
		gui_post(popup_disconnected_cb, NULL);
	}
}

// Takes the card back from the host if it was handed over, from any thread.
static void take_card_back(const char *why) {
	pthread_mutex_lock(&storage_lock);
	if (storage_active) {
		fprintf(stderr, "usb: %s; taking the card back from the host\n", why);
		storage_restore();
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
		storage_restore();
	}
	pthread_mutex_unlock(&storage_lock);
}

static void *usb_thread(void *arg) {
	(void)arg;
	thread_be_background("usb watcher");

	bool exported = false;
	// Set after the PC ejects the medium: the card is back but the cable is
	// still in, and re-exporting now would undo the eject the user just asked
	// for. Cleared when the host goes away -- VBUS drops, or the UDC leaves
	// "configured" even if a charger node still claims a cable.
	bool await_unplug = false;

	// Said once, not once a second.
	bool said_adb = false;

	// And once for a host that turns up while the Apps feature holds the card.
	bool said_apps = false;

	// The gadget is unbound from the controller while the device sits in
	// standby with no cable (see below).
	bool parked = false;

	// The previous poll's enumeration state. A falling edge is a host going
	// away, and that is a new session even if a PMIC node still says VBUS.
	bool was_configured = false;

	unsigned seen_owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);

	for (;;) {
		// DAC mode owns the controller: stand completely down. Not just "do
		// not rebuild" -- do not look, do not export, do not park. Everything
		// this loop does ends in a write to the gadget, and every one of those
		// writes would land on the audio gadget instead.
		if (dac_owns_usb) {
			sleep(1);
			continue;
		}

		// The power thread is between unbinding the gadget and writing mem.
		// Anything this loop did now could put a gadget back on the controller
		// and turn the suspend into a reset.
		if (suspend_in_progress) {
			sleep(1);
			continue;
		}

		// And the same for a DAC or a pair of headphones on the port: the
		// controller is the host's then, and everything below ends in a write
		// to a gadget that has no business existing while it is.
		if (usbaudio_active()) {
			sleep(1);
			continue;
		}

		// Slower while the panel is dark. Keep this short even then: the
		// gadget is unbound while the screen is off with no cable, so this is
		// how long a newly attached host sees nothing at all.
		sleep(power_screen_is_on() ? POLL_SECONDS : POLL_SECONDS_STANDBY);

		// DAC mode or a USB audio device can take the controller during the
		// sleep above. Recheck before touching any gadget: this window is how a
		// storage rebind used to land on top of the audio gadget.
		if (dac_owns_usb || usbaudio_active()) {
			continue;
		}

		// The controller changed hands since the last look: whatever was
		// known about the cable belongs to the gadget before. And a card
		// taken back by DAC mode or ADB is not exported any more, whatever
		// this loop last did with it: believing otherwise read the emptied
		// medium as an eject by the host, and the card was not offered again
		// until the cable was pulled.
		unsigned owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);
		if (owner != seen_owner) {
			seen_owner = owner;
			await_unplug = false;
			said_adb = false;
			said_apps = false;
			// A transition may have bound the reader for us. Only believe the
			// local parked flag while the UDC really is empty.
			if (parked && storage_gadget_bound()) {
				parked = false;
			}
		}
		if (exported && !storage_active) {
			exported = false;
		}

		adb_keep_controller();

		// A host that has enumerated this device is a cable, whatever the
		// charger says. On a phone the two can disagree: the power role and the
		// data role are separate on Type-C, so a phone can act as the host
		// while the charger node reads nothing at all -- and treating that as
		// "no cable" is how the gadget ended up being unbound from under a host
		// that was talking to it.
		bool configured = udc_configured();
		if (was_configured && !configured) {
			// The enumerated host just went away. Some charger nodes stay high
			// after an unplug, which used to leave said_apps latched: the next
			// plug-in was then answered with silence instead of the "close the
			// app?" prompt. A new enumeration is a new session, and a fresh
			// plug-in gets an immediate export attempt.
			await_unplug = false;
			said_apps = false;
			said_adb = false;
			export_retry_at = 0;
		}
		was_configured = configured;
		bool vbus = vbus_present() || configured;

		if (!vbus) {
			if (exported) {
				locked_restore();
				exported = false;
			}
			await_unplug = false;
			said_apps = false;
			export_retry_at = 0;

			// No cable, panel dark: take the gadget off the controller. A
			// bound gadget keeps the USB PHY and the OTG block clocked --
			// /sys/kernel/debug/clk/clk_summary shows gate_usbphy and
			// gate_otg alive a minute into standby with nothing plugged in --
			// and that is current spent waiting for a cable that is not there.
			// The stock player only ever builds a gadget once a host has
			// turned up; this is the same idea, but only in standby, so
			// nothing about plugging a cable into a device in use changes.
			//
			// Never while ADB is running: the controller is ADB's then, and
			// reclaiming it would cut the connection.
			//
			// And not while audio is open either. Both of these are
			// housekeeping for a cable that is not attached: nothing is
			// waiting for the gadget, so there is no reason to touch the
			// controller in the middle of a track -- a rebind runs the whole
			// gadget script, shell and all, and costs a hole in the sound.
			// Both happen at the next poll after playback stops, and a cable
			// turning up is handled below whatever this decided.
			if (audio_device_is_open()) {
				continue;
			}

			if (!parked && !power_screen_is_on() && !adb_owns_controller()) {
				gadget_unbind();
				parked = true;
				fprintf(stderr, "usb: no cable and the screen is dark; gadget unbound\n");
			} else if (parked && power_screen_is_on()) {
				// After mem the reader must stay off until a cable turns up; see
				// usb_resume_restore(). Binding it now is exactly what stalled
				// dwc2 for the next host.
				if (rebind_on_cable) {
					continue;
				}
				// Unless ADB has been switched on meanwhile: the controller is
				// its gadget's now, and adb_keep_controller() sees to it.
				if (!adb_owns_controller() && !gadget_rebind()) {
					ensure_storage_gadget();
				}
				parked = false;
				fprintf(stderr, "usb: awake again; gadget rebound\n");
			}
			continue;
		}

		// The first cable after mem: build the reader now, with the host already
		// watching. A UDC toggle at this moment is what the old ADB workaround
		// was doing by hand, and it is what gets dwc2 out of the stalled setup
		// queue it had when the gadget was bound before the cable.
		if (rebind_on_cable) {
			rebind_on_cable = false;
			if (adb_owns_controller()) {
				fprintf(stderr, "usb: cable after resume with ADB on; ADB keeps the controller\n");
				continue;
			}
			fprintf(stderr, "usb: first cable after resume; building the reader\n");
			typec_force_sink();
			gadget_unbind(); // clear any stale UDC binding from before mem
			usleep(120 * 1000);
			ensure_storage_gadget();
			parked = false;
			continue; // one poll for the host to enumerate
		}

		// A bound empty reader is what the host enumerates while we decide
		// whether the card can be exported. If a transition or a failed rebind
		// left the gadget off the controller, put it back now rather than
		// waiting for a state change that will never come.
		if (!parked && !storage_gadget_bound() && !adb_owns_controller()) {
			typec_force_sink();
			if (exported) {
				// The card is still supposed to be exported. Put it back with the
				// medium in, not as an empty reader that the host would treat as
				// an eject. If that keeps failing, give the card back to the
				// player rather than leaving it unmounted with nobody serving it.
				bool ok = storage_present_medium(STORAGE_GADGET "/functions/mass_storage.0/lun.0/file");
				fprintf(stderr, "usb: exported reader fell off the controller; %s\n",
						ok ? "rebound with the medium" : "could not be restored");
				if (!ok) {
					locked_restore();
					exported = false;
					export_retry_at = time(NULL) + 10;
				}
			} else {
				ensure_storage_gadget();
				fprintf(stderr, "usb: storage gadget was unbound with a cable present; rebuilt%s\n",
						storage_gadget_bound() ? "" : " (FAILED)");
			}
			continue; // give the controller a poll to enumerate
		}

		// A cable turned up while the gadget was parked: put it back before
		// anything else, or the host will never see this device.
		if (parked) {
			// Even while the Apps guard is up. That guard stops the card being
			// inserted, not the empty reader being enumerated; without the
			// enumeration the watcher cannot tell a PC is there and the prompt to
			// close the app would never be posted. This was the second-plug-in
			// failure: unplug with the screen dark (the gadget parks), open or
			// keep an app, plug in again -- and nothing happened.
			//
			if (dac_owns_usb || usbaudio_active()) {
				continue;
			}
			// The recovery above may already have bound it. Do not unbind and
			// rebind a working reader just because the local flag said parked.
			typec_force_sink();
			if (!storage_gadget_bound() && !adb_owns_controller() && !gadget_rebind()) {
				ensure_storage_gadget();
			}
			parked = false;
			fprintf(stderr, "usb: cable in; gadget %s%s\n", storage_gadget_bound() ? "ready" : "FAILED to bind",
					apps_own_usb ? " (apps guard up; the host will be asked)" : "");
			continue; // give the controller a poll to enumerate
		}

		if (exported) {
			if (host_ejected()) {
				fprintf(stderr, "usb: host ejected the medium, reclaiming the card\n");
				locked_restore();
				exported = false;
				await_unplug = true;
			} else if (!configured && !storage_gadget_bound() && !dac_owns_usb &&
					   !adb_owns_controller()) {
				// The medium is still in the lun, but the gadget is no longer on
				// the controller: a stopped ADB gadget, a failed rebind or a
				// suspend/resume race. The host is still on the cable, so put the
				// reader back instead of waiting forever for a restore that will
				// never come.
				typec_force_sink();
				if (!gadget_rebind()) {
					ensure_storage_gadget();
				}
				fprintf(stderr, "usb: exported gadget fell off the controller; rebound\n");
			}
			continue;
		}

		if (await_unplug) {
			continue;
		}

		// VBUS alone is not a PC -- a wall charger raises it too, and taking
		// the card away (stopping the music with it) for a power brick was
		// exactly the "track would not start while charging" bug. Only a host
		// that has enumerated the gadget counts.
		if (!configured) {
			continue;
		}

		// The Apps feature has a package mounted from this card. Ask before
		// taking the card: confirming closes the app (and waits for a tool to
		// stop), turns ADB off if it was on, leaves the Apps page and lets the
		// watcher export below. Asked before the ADB check so a host that is
		// there for storage is not silently answered with a shell instead.
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

		// ADB is on: the cable is here to carry a shell, not to be a card
		// reader. Exporting the card now unmounts it from under that shell --
		// which is why /mnt/sd_0 came back "Read-only file system" over ADB --
		// and the stock firmware does not do it either. Leave the card where
		// it is. The Apps prompt above is the way out: confirming USB storage
		// switches ADB off first.
		if (adb_owns_controller()) {
			if (!said_adb) {
				fprintf(stderr, "usb: ADB is on; the card stays mounted (no mass storage)\n");
				said_adb = true;
			}
			continue;
		}
		said_adb = false;

		// A previous export failed while the holder of the card was still
		// there. Do not unmount/remount it once a second; give it a moment.
		// Asked after the prompt checks so a USB plug-in is never silently
		// ignored just because the last export failed.
		if (export_retry_at && time(NULL) < export_retry_at) {
			continue;
		}

		locked_export();
		exported = storage_active;
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
	usb_started = true;

	// The empty-medium gadget goes up right away, unless ADB owns the
	// controller -- then its gadget provides the enumeration signal instead.
	// "Owns" includes the moment before adbd has bound it: started just
	// before this, it may not have got that far, and a card reader bound now
	// would keep it off the controller (see adb_keep_controller()).
	if (!adb_gadget_bound() && !adb_owns_controller()) {
		ensure_storage_gadget();
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, usb_thread, NULL) == 0) {
		pthread_detach(thread);
		fprintf(stderr, "usb: watching for a USB host (exporting %s)\n", sd_device);
	}
}
