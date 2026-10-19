#ifndef SONIXLINK_BT_H
#define SONIXLINK_BT_H

#include <stdbool.h>

// SonixLink over Bluetooth: the same HTTP requests as over Wi-Fi, carried on
// an RFCOMM link instead of a TCP connection, for a phone paired with the
// player.
//
// The service is registered with bluez as a Profile1 under SONIXLINK_BT_UUID,
// so the phone finds its channel through SDP. bluez accepts the link and hands
// the socket over in Profile1.NewConnection; the socket goes to the SonixLink
// worker (sonixlink_adopt_link), which serves it like any other client but
// keeps it open between requests.
//
// Registered while SonixLink is on and Bluetooth is on, gone otherwise.

// The service UUID the app connects to. Written the same in the app.
#define SONIXLINK_BT_UUID "8d6e3a52-4c1f-4b7e-9a2d-5f0c7e1b3a90"

// The RFCOMM channel it listens on, also written in the app: the app looks the
// service up by UUID and falls back to this number.
#define SONIXLINK_BT_CHANNEL 22

// Starts the thread that keeps the registration in step with the two switches.
void sonixlink_bt_init(void);

// Whether `mac` ("AA:BB:...") is a phone that has opened a SonixLink link since
// the player started. Such a phone is a remote control, not a pair of
// headphones: the Bluetooth code leaves it out when it looks for the connected
// audio device, and never disconnects it to make room for one.
bool sonixlink_bt_is_peer(const char *mac);

#endif /* SONIXLINK_BT_H */
