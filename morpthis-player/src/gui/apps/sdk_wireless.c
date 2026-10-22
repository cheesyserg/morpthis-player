#include "sdk_internal.h"

#include <stdio.h>
#include <string.h>

#include "src/system/bluetooth/bluetooth.h"
#include "src/system/net/wifi.h"

// ---------------------------------------------------------------------------
// wireless.h: Wi-Fi
// ---------------------------------------------------------------------------

static morpthis_wifi_state_t wifi_state_from_internal(wifi_state_t state) {
	switch (state) {
	case WIFI_STATE_ON:
		return MORPTHIS_WIFI_ON;
	case WIFI_STATE_CONNECTING:
		return MORPTHIS_WIFI_CONNECTING;
	case WIFI_STATE_CONNECTED:
		return MORPTHIS_WIFI_CONNECTED;
	case WIFI_STATE_OFF:
	default:
		return MORPTHIS_WIFI_OFF;
	}
}

static morpthis_wifi_op_t wifi_op_from_internal(wifi_op_t op) {
	switch (op) {
	case WIFI_OP_BUSY:
		return MORPTHIS_WIFI_OP_BUSY;
	case WIFI_OP_OK:
		return MORPTHIS_WIFI_OP_OK;
	case WIFI_OP_FAILED:
		return MORPTHIS_WIFI_OP_FAILED;
	case WIFI_OP_IDLE:
	default:
		return MORPTHIS_WIFI_OP_IDLE;
	}
}

static morpthis_wifi_fail_t wifi_fail_from_internal(wifi_fail_t fail) {
	switch (fail) {
	case WIFI_FAIL_WRONG_KEY:
		return MORPTHIS_WIFI_FAIL_WRONG_KEY;
	case WIFI_FAIL_REFUSED:
		return MORPTHIS_WIFI_FAIL_REFUSED;
	case WIFI_FAIL_OTHER:
		return MORPTHIS_WIFI_FAIL_OTHER;
	case WIFI_FAIL_NONE:
	default:
		return MORPTHIS_WIFI_FAIL_NONE;
	}
}

static morpthis_wifi_opkind_t wifi_opkind_from_internal(wifi_opkind_t kind) {
	switch (kind) {
	case WIFI_OPKIND_DISCONNECT:
		return MORPTHIS_WIFI_OPKIND_DISCONNECT;
	case WIFI_OPKIND_FORGET:
		return MORPTHIS_WIFI_OPKIND_FORGET;
	case WIFI_OPKIND_CONNECT:
	default:
		return MORPTHIS_WIFI_OPKIND_CONNECT;
	}
}

bool morpthis_wifi_available(void) { return wifi_available(); }

bool morpthis_wifi_enabled(void) { return wifi_get_enabled(); }

void morpthis_wifi_set_enabled(bool on) { wifi_set_enabled(on); }

bool morpthis_wifi_busy(void) { return wifi_busy(); }

void morpthis_wifi_status(morpthis_wifi_status_t *out) {
	if (!out) {
		return;
	}
	wifi_status_t status;
	wifi_get_status(&status);
	memset(out, 0, sizeof(*out));
	out->state = wifi_state_from_internal(status.state);
	snprintf(out->ssid, sizeof(out->ssid), "%s", status.ssid);
	snprintf(out->ip, sizeof(out->ip), "%s", status.ip);
	snprintf(out->mac, sizeof(out->mac), "%s", status.mac);
	out->signal = status.signal;
	out->bars = status.bars;
}

bool morpthis_wifi_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

void morpthis_wifi_scan(void) { wifi_scan_start(); }

bool morpthis_wifi_scan_running(void) { return wifi_scan_running(); }

unsigned morpthis_wifi_networks_serial(void) { return wifi_networks_serial(); }

int morpthis_wifi_networks(morpthis_wifi_network_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	wifi_network_t networks[WIFI_MAX_NETWORKS];
	if (max > WIFI_MAX_NETWORKS) {
		max = WIFI_MAX_NETWORKS;
	}
	int count = wifi_get_networks(networks, max);
	for (int i = 0; i < count; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", networks[i].ssid);
		out[i].signal = networks[i].signal;
		out[i].bars = networks[i].bars;
		out[i].secured = networks[i].secured;
		out[i].saved = networks[i].saved;
		out[i].current = networks[i].current;
		out[i].id = networks[i].id;
	}
	return count;
}

void morpthis_wifi_connect(const char *ssid, const char *psk) {
	if (ssid && ssid[0]) {
		wifi_connect(ssid, psk);
	}
}

void morpthis_wifi_connect_hidden(const char *ssid, const char *psk) {
	if (ssid && ssid[0]) {
		wifi_connect_hidden(ssid, psk);
	}
}

void morpthis_wifi_disconnect(void) { wifi_disconnect(); }

void morpthis_wifi_forget(const char *ssid) {
	if (ssid && ssid[0]) {
		wifi_forget(ssid);
	}
}

morpthis_wifi_op_t morpthis_wifi_take_result(char *ssid_out, size_t size) {
	return wifi_op_from_internal(wifi_take_op_result(ssid_out, (int)size));
}

morpthis_wifi_fail_t morpthis_wifi_last_failure(void) { return wifi_fail_from_internal(wifi_last_failure()); }

morpthis_wifi_opkind_t morpthis_wifi_last_op_kind(void) {
	return wifi_opkind_from_internal(wifi_last_op_kind());
}

// ---------------------------------------------------------------------------
// wireless.h: Bluetooth
// ---------------------------------------------------------------------------

static morpthis_bt_state_t bt_state_from_internal(bt_state_t state) {
	switch (state) {
	case BT_STATE_ON:
		return MORPTHIS_BT_ON;
	case BT_STATE_CONNECTED:
		return MORPTHIS_BT_CONNECTED;
	case BT_STATE_OFF:
	default:
		return MORPTHIS_BT_OFF;
	}
}

static morpthis_bt_op_t bt_op_from_internal(bt_op_t op) {
	switch (op) {
	case BT_OP_BUSY:
		return MORPTHIS_BT_OP_BUSY;
	case BT_OP_OK:
		return MORPTHIS_BT_OP_OK;
	case BT_OP_PAIRED:
		return MORPTHIS_BT_OP_PAIRED;
	case BT_OP_FAILED:
		return MORPTHIS_BT_OP_FAILED;
	case BT_OP_IDLE:
	default:
		return MORPTHIS_BT_OP_IDLE;
	}
}

static void bt_device_copy(morpthis_bt_device_t *out, const bt_device_t *in) {
	memset(out, 0, sizeof(*out));
	snprintf(out->mac, sizeof(out->mac), "%s", in->mac);
	snprintf(out->name, sizeof(out->name), "%s", in->name);
	out->paired = in->paired;
	out->connected = in->connected;
	out->trusted = in->trusted;
	out->rssi = in->rssi;
}

bool morpthis_bluetooth_available(void) { return bluetooth_available(); }

bool morpthis_bluetooth_enabled(void) { return bluetooth_get_enabled(); }

void morpthis_bluetooth_set_enabled(bool on) { bluetooth_set_enabled(on); }

bool morpthis_bluetooth_busy(void) { return bluetooth_busy(); }

morpthis_bt_state_t morpthis_bluetooth_state(void) {
	return bt_state_from_internal(bluetooth_get_state());
}

const char *morpthis_bluetooth_local_name(void) { return bluetooth_local_name(); }

bool morpthis_bluetooth_set_local_name(const char *name) {
	return name && bluetooth_set_local_name(name);
}

unsigned morpthis_bluetooth_devices_serial(void) { return bluetooth_devices_serial(); }

int morpthis_bluetooth_paired(morpthis_bt_device_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	bt_device_t devices[BT_MAX_DEVICES];
	if (max > BT_MAX_DEVICES) {
		max = BT_MAX_DEVICES;
	}
	int count = bluetooth_get_paired(devices, max);
	for (int i = 0; i < count; i++) {
		bt_device_copy(&out[i], &devices[i]);
	}
	return count;
}

int morpthis_bluetooth_found(morpthis_bt_device_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	bt_device_t devices[BT_MAX_DEVICES];
	if (max > BT_MAX_DEVICES) {
		max = BT_MAX_DEVICES;
	}
	int count = bluetooth_get_found(devices, max);
	for (int i = 0; i < count; i++) {
		bt_device_copy(&out[i], &devices[i]);
	}
	return count;
}

bool morpthis_bluetooth_connected_device(morpthis_bt_device_t *out) {
	if (!out) {
		return false;
	}
	bt_device_t device;
	if (!bluetooth_connected_device(&device)) {
		return false;
	}
	bt_device_copy(out, &device);
	return true;
}

bool morpthis_bluetooth_audio_active(void) { return bluetooth_audio_active(); }

void morpthis_bluetooth_scan_start(void) { bluetooth_scan_start(); }

void morpthis_bluetooth_scan_stop(void) { bluetooth_scan_stop(); }

bool morpthis_bluetooth_scan_running(void) { return bluetooth_scan_running(); }

void morpthis_bluetooth_refresh(void) { bluetooth_refresh(); }

void morpthis_bluetooth_pair(const char *mac) {
	if (mac && mac[0]) {
		bluetooth_pair(mac);
	}
}

void morpthis_bluetooth_connect(const char *mac) {
	if (mac && mac[0]) {
		bluetooth_connect(mac);
	}
}

void morpthis_bluetooth_disconnect(const char *mac) {
	if (mac && mac[0]) {
		bluetooth_disconnect(mac);
	}
}

void morpthis_bluetooth_forget(const char *mac) {
	if (mac && mac[0]) {
		bluetooth_forget(mac);
	}
}

morpthis_bt_op_t morpthis_bluetooth_take_result(char *name_out, size_t size) {
	return bt_op_from_internal(bluetooth_take_op_result(name_out, (int)size));
}
