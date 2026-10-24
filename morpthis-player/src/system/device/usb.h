#ifndef USB_H
#define USB_H

#include <stdbool.h>

// USB mass storage, the way the stock firmware does it.
//
// When a PC is attached, the stock player unmounts the card (through
// sys_server, "MOUNT:UMOUNT:..."), builds a configfs gadget and hands the raw
// block device to the host with the firmware's own script,
// /usr/bin/usb_dev_mass_storage.sh. On unplug it tears the gadget down and
// remounts. This does exactly that -- the scripts and the daemon are already
// on the device; the player's job is only to notice the cable and call them
// in the right order.
//
// Detection polls /sys/class/power_supply/usb/online from a small background
// thread. A wall charger raises the same signal; the reader is bound while the
// cable is in, but nothing is exported until the UDC reports that a host has
// actually enumerated it.

// Starts the watcher thread. Pass the SD block device (e.g. /dev/mmcblk0p1)
// and the mount point it lives on; either NULL disables the whole feature
// (host build).
void usb_start(const char *sd_device, const char *sd_mount_point);

// True while the gadget is exported to a host (the card is unmounted then).
bool usb_storage_active(void);

// Hands the USB controller to DAC mode, and takes it back.
//
// While it is handed over, everything in usb.c stands down: no gadget is
// rebuilt, the card is not exported, and the watcher thread does not touch the
// controller at all. Without this the watcher sees a cable, rebuilds the
// mass-storage gadget over the audio one, and the computer both loses the
// audio stream and gains a microSD it should not have.
void usb_set_dac_mode(bool on);
bool usb_dac_mode(void);

// Keeps the card out of mass-storage mode while the Apps feature is in use.
//
// An app runs from a squashfs image on the card, so handing the raw device to
// a PC would unmount the card out from under it. Setting this takes a card
// already exported back first; clearing it lets the watcher export again on
// its next poll. The Apps page, a loaded app and a running tool each hold the
// guard, so it stays on until the last of them is gone.
//
// Only the medium is held back. The empty card reader stays enumerated while
// the guard is up: a host has to be able to see the device for the watcher to
// tell a PC from a charger and put up the "close the app and share the card?"
// prompt.
void usb_set_apps_mode(bool on);

// Coordination with ADB, whose init script needs the one USB controller for
// its own gadget: yield takes the card back and unbinds the reader before adbd
// starts; reclaim releases ADB's gadget and lets the watcher rebuild the
// reader if a host is still on the cable. Called by adb.c around the init
// scripts.
void usb_gadget_yield_to_adb(void);
void usb_gadget_reclaim_from_adb(void);

// True while a cable is supplying VBUS (a PC or a charger). Read straight
// from the power-supply nodes, so it is current rather than cached.
bool usb_vbus_present(void);

// Around a system suspend. The reader may be pre-positioned while the screen
// is on, so prepare() takes a card a host still has and unbinds every gadget; a
// gadget left on the controller makes this SoC answer with a reset instead of a
// resume. restore() only clears the stand-down and defers building anything
// until the first cable.
void usb_suspend_prepare(void);
void usb_resume_restore(void);

// The product string: the model's name without the maker's
const char *usb_product(void);

#endif /* USB_H */
