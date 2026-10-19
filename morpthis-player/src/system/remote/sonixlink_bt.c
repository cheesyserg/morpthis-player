#include "sonixlink_bt.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/dbuslite.h"
#include "src/system/core/utils.h"
#include "src/system/remote/sonixlink.h"

#define BLUEZ_NAME "org.bluez"
#define PROFILE_MANAGER_PATH "/org/bluez"
#define PROFILE_MANAGER "org.bluez.ProfileManager1"
#define PROFILE_IFACE "org.bluez.Profile1"
#define PROFILE_PATH "/org/morpthis/sonixlink"

// The RFCOMM channel the service listens on. bluez opens a listening socket
// for an external profile only when it is given a channel or a PSM: a custom
// UUID has no default, and without one the profile registers, the SDP record
// goes up without a channel in it, and every connection is refused. Fixed
// rather than automatic so the app can still reach it straight by number when
// a phone's SDP lookup fails. Well away from channel 1, which HiByLink's stock
// record uses.
#define RFCOMM_CHANNEL SONIXLINK_BT_CHANNEL

#define POLL_MS 2000
// After bluez has refused, or is not there yet.
#define RETRY_MS 5000
// From bluetoothd appearing to registering with it: its adapter and its
// ProfileManager1 come up a moment after the name does.
#define BLUEZ_SETTLE_MS 1500
// How often the registration is checked by registering again: bluez answers
// AlreadyExists while the profile is there, and takes it afresh when it is
// not, whatever made it go without a word to this side.
#define CHECK_MS 30000

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static pthread_t worker;
static bool worker_started;
static dbus_conn_t *conn;
static bool registered;
static uint32_t checked_ms;

// Set from the reader thread when org.bluez changes owner. A profile lives in
// the bluetoothd that registered it, and a new bluetoothd -- the stack brought
// up again after a park, a resume or a restart, with the switch on throughout
// -- has none: no SDP record, no listening channel, every phone refused. The
// worker registers again on the new one.
static volatile bool bluez_moved;

static void on_signal(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)c;
	(void)user;
	if (strcmp(m->member, "NameOwnerChanged") == 0) {
		bluez_moved = true;
	}
}

// The phones that have opened a SonixLink link since the player started, by
// address. The Bluetooth code reads this to leave them out of what it counts
// as connected headphones.
#define PEERS_MAX 4
static pthread_mutex_t peers_lock = PTHREAD_MUTEX_INITIALIZER;
static char peers[PEERS_MAX][18];
static int peer_count;

// "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" -> "AA:BB:CC:DD:EE:FF".
static void remember_peer(const char *device_path) {
	const char *at = strstr(device_path, "dev_");
	if (!at || strlen(at + 4) < 17) {
		return;
	}
	char mac[18];
	for (int i = 0; i < 17; i++) {
		char ch = at[4 + i];
		mac[i] = ch == '_' ? ':' : ch;
	}
	mac[17] = '\0';

	pthread_mutex_lock(&peers_lock);
	bool known = false;
	for (int i = 0; i < peer_count && !known; i++) {
		known = strcasecmp(peers[i], mac) == 0;
	}
	if (!known) {
		if (peer_count == PEERS_MAX) {
			memmove(peers[0], peers[1], sizeof(peers[0]) * (PEERS_MAX - 1));
			peer_count--;
		}
		memcpy(peers[peer_count++], mac, sizeof(mac));
	}
	pthread_mutex_unlock(&peers_lock);
}

bool sonixlink_bt_is_peer(const char *mac) {
	if (!mac || !mac[0]) {
		return false;
	}
	pthread_mutex_lock(&peers_lock);
	bool found = false;
	for (int i = 0; i < peer_count && !found; i++) {
		found = strcasecmp(peers[i], mac) == 0;
	}
	pthread_mutex_unlock(&peers_lock);
	return found;
}

// bluez calling the profile: a phone has connected (NewConnection, with the
// RFCOMM socket), wants to disconnect, or the profile is being released.
static void on_call(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)user;
	if (strcmp(m->path, PROFILE_PATH) != 0 || strcmp(m->interface, PROFILE_IFACE) != 0) {
		dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown");
		return;
	}

	if (strcmp(m->member, "NewConnection") == 0) {
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char device[DBUS_NAME_MAX] = "";
		uint32_t index = 0;
		dbus_r_string(&r, device, sizeof(device));
		bool ok = dbus_r_u32(&r, &index);
		int fd = ok ? dbus_take_fd(c, index) : -1;
		if (fd < 0) {
			dbus_error(c, m, "org.bluez.Error.Rejected", "no socket");
			return;
		}
		dbus_reply_begin(c, m, "");
		dbus_reply_send(c);
		printf("sonixlink: Bluetooth link from %s\n", device);
		remember_peer(device);
		sonixlink_adopt_link(fd);
		return;
	}

	if (strcmp(m->member, "Release") == 0) {
		registered = false;
	}
	// RequestDisconnection and Release need nothing more than an answer: the
	// worker notices a closed socket by itself.
	dbus_reply_begin(c, m, "");
	dbus_reply_send(c);
}

// True when bluez has the profile after the call: registered now, or already
// (`already` then set).
static bool register_profile(bool *already) {
	if (already) {
		*already = false;
	}
	dbus_writer_t *w =
		dbus_call_begin(conn, BLUEZ_NAME, PROFILE_MANAGER_PATH, PROFILE_MANAGER, "RegisterProfile", "osa{sv}");
	dbus_w_path(w, PROFILE_PATH);
	dbus_w_string(w, SONIXLINK_BT_UUID);
	dbus_array_t options;
	dbus_w_array_begin(w, "{sv}", &options);
	dbus_w_dict_string(w, "Name", "SonixLink");
	dbus_w_dict_string(w, "Role", "server");
	dbus_w_dict_variant_begin(w, "Channel", "q");
	dbus_w_u16(w, RFCOMM_CHANNEL);
	// A paired phone is enough: nothing asks the user each time it connects.
	dbus_w_dict_bool(w, "RequireAuthentication", false);
	dbus_w_dict_bool(w, "RequireAuthorization", false);
	dbus_w_array_end(w, &options);

	char err[DBUS_NAME_MAX] = "";
	if (dbus_call_send(conn, 5000, err, sizeof(err))) {
		return true;
	}
	if (strstr(err, "AlreadyExists")) {
		if (already) {
			*already = true;
		}
		return true;
	}
	fprintf(stderr, "sonixlink: RegisterProfile refused (%s)\n", err[0] ? err : "no reply");
	return false;
}

static void teardown(void) {
	if (!conn) {
		return;
	}
	if (registered && dbus_alive(conn)) {
		dbus_writer_t *w =
			dbus_call_begin(conn, BLUEZ_NAME, PROFILE_MANAGER_PATH, PROFILE_MANAGER, "UnregisterProfile", "o");
		dbus_w_path(w, PROFILE_PATH);
		dbus_call_send(conn, 2000, NULL, 0);
	}
	dbus_disconnect(conn);
	conn = NULL;
	registered = false;
}

static void *worker_func(void *unused) {
	(void)unused;
	thread_be_background("sonixlink bt");

	for (;;) {
		bool wanted = sonixlink_get_enabled() && bluetooth_get_enabled();
		if (!wanted) {
			if (conn) {
				teardown();
				printf("sonixlink: Bluetooth service withdrawn\n");
			}
			usleep(POLL_MS * 1000);
			continue;
		}

		if (conn && bluez_moved) {
			printf("sonixlink: bluetoothd restarted; registering the Bluetooth service again\n");
			teardown();
			usleep(BLUEZ_SETTLE_MS * 1000);
		}
		if (conn && (!dbus_alive(conn) || !registered)) {
			teardown();
		}
		if (!conn) {
			conn = dbus_connect_system_fds(NULL, on_call, NULL);
			if (conn) {
				dbus_set_signal_handler(conn, on_signal, NULL);
				dbus_add_match(conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
									 "member='NameOwnerChanged',arg0='" BLUEZ_NAME "'");
				bluez_moved = false;
				registered = register_profile(NULL);
				checked_ms = now_ms();
				if (registered) {
					printf("sonixlink: Bluetooth service registered (%s, RFCOMM channel %d)\n", SONIXLINK_BT_UUID,
					   RFCOMM_CHANNEL);
				} else {
					teardown();
				}
			}
			if (!conn) {
				usleep(RETRY_MS * 1000);
				continue;
			}
		}
		if (registered && (uint32_t)(now_ms() - checked_ms) >= CHECK_MS) {
			checked_ms = now_ms();
			bool already = false;
			if (!register_profile(&already)) {
				registered = false;
			} else if (!already) {
				printf("sonixlink: the Bluetooth service had gone from bluez; registered again\n");
			}
		}
		usleep(POLL_MS * 1000);
	}
	return NULL;
}

void sonixlink_bt_init(void) {
	if (worker_started) {
		return;
	}
	if (pthread_create(&worker, NULL, worker_func, NULL) != 0) {
		fprintf(stderr, "sonixlink: no thread for the Bluetooth service\n");
		return;
	}
	pthread_detach(worker);
	worker_started = true;
}
