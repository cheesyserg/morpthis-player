#ifndef BTLOG_H
#define BTLOG_H

#include <stdbool.h>

// The Bluetooth log: everything about a connection that fails, in one file on
// the card that anybody can send without ADB.
//
// Three sources, one timeline, each line stamped like the player's own log:
//
//   - btmon, the HCI traffic between bluez and the chip: who paged whom, which
//     L2CAP channel was refused, every AVDTP command and its answer. Run on a
//     pseudo-terminal, so it writes a line at a time rather than four
//     kilobytes at a time -- a pipe would leave the last packets before a
//     failure sitting in its buffer.
//   - bluetoothd and bluealsa, through syslog. There is no syslogd on this
//     firmware; while the log is on, /dev/log is this module's socket. bluez
//     writes to syslog whatever its command line, so this needs no restart, and
//     a socket left without a reader (the player gone) only makes those writes
//     fail -- it never blocks the daemon.
//   - the player's own Bluetooth lines ("bluetooth:", "btstack:" and the rest),
//     copied from its log as they are printed, whether or not that log is on.
//
// The audio itself is left out. Streaming is a couple of hundred ACL packets a
// second, each with its acknowledgement, and all they would say is "audio
// packet"; a line counting how many were skipped takes their place.
//
// Written to .local/bluetooth.log on the card. At 8 MB it becomes
// bluetooth.old.log and a new one starts, so it never takes more than 16.
//
// Off by default, under Settings > Developer options > Bluetooth log, and
// remembered across restarts.

// Starts the log if the setting asks for it. Call once, after the config has
// been read and before Bluetooth is brought up.
void btlog_init(void);

bool btlog_enabled(void);
void btlog_set_enabled(bool enabled);

// The file being written, for the settings page; a short text when there is
// no card or the log is off.
const char *btlog_path(void);

// The card under the file. Release before anything unmounts it (an open
// descriptor makes the unmount fail); attach once it is back, with its root.
// Lines printed while it is away are counted and the count is written when it
// returns.
void btlog_card_release(void);
void btlog_card_attach(const char *sd_root);

#endif /* BTLOG_H */
