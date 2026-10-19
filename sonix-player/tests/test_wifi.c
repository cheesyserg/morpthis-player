/*
 * The Wi-Fi worker's half of joining a hidden network, against a fake
 * wpa_cli: the one command that makes a hidden network joinable at all is
 * set_network <id> scan_ssid 1, and it has to be written for a network the
 * supplicant already knows as well as for one created on the spot -- and
 * never for an ordinary, visible network, whose name is already in the air.
 *
 * wifi.c speaks to wpa_cli through execvp, so the test puts a small shell
 * script of that name first on PATH. It logs every argv it is handed and
 * answers the way a cooperative supplicant would (every set accepted, the
 * link COMPLETED at the first poll). Everything else wifi.c leans on -- the
 * config store, sys_server, the power and clock helpers -- is stubbed below;
 * sys_server reports itself absent, which is also what keeps the worker on
 * the direct wpa_cli route this test is about.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/device/sysserver.h"
#include "src/system/net/wifi.h"

/* ---- the parts of the player wifi.c leans on, substituted ---------------- */

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	(void)fallback;
	return 0; // Wi-Fi left off: the worker starts, but brings nothing up
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

bool sysserver_available(void) { return false; }

int sysserver_request(const char *command, char *reply, size_t reply_size) {
	(void)command;
	(void)reply;
	(void)reply_size;
	return -1;
}

bool power_screen_is_on(void) { return true; }

void clock_network_sync(void) {}

void thread_be_background(const char *name) { (void)name; }

void deadline_in_ms(struct timespec *out, unsigned int ms) {
	clock_gettime(CLOCK_REALTIME, out);
	out->tv_sec += (time_t)(ms / 1000);
	out->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (out->tv_nsec >= 1000000000L) {
		out->tv_sec++;
		out->tv_nsec -= 1000000000L;
	}
}

/* ---- the fake wpa_cli ----------------------------------------------------- */

static char log_path[512];
static char saved_path[512];

static const char FAKE_WPA_CLI[] =
	"#!/bin/sh\n"
	"printf '%s|' \"$@\" >> \"$FAKE_WPA_LOG\"; printf '\\n' >> \"$FAKE_WPA_LOG\"\n"
	"shift 2\n" // -i wlan0
	"case \"$1\" in\n"
	"  list_networks)\n"
	"    if [ -f \"$FAKE_WPA_SAVED\" ]; then printf '0\\tSavedHidden\\tany\\t[CURRENT]\\n'\n"
	"    else printf 'network id\\tssid\\tbssid\\tflags\\n'; fi;;\n"
	"  add_network) printf '0\\n';;\n"
	"  status) printf 'wpa_state=COMPLETED\\nssid=Whatever\\nip_address=192.168.1.50\\naddress=aa:bb:cc:dd:ee:ff\\n';;\n"
	"  signal_poll) printf 'RSSI=-52\\n';;\n"
	"  scan_results)\n"
	"    printf 'bssid / frequency / signal / flags / ssid\\n'\n"
	"    if [ -n \"$FAKE_WPA_SCAN_SSID\" ]; then\n"
	"      printf 'aa:bb:cc:dd:ee:ff\\t2412\\t-52\\t%s\\t%s\\n' \"$FAKE_WPA_SCAN_FLAGS\" \"$FAKE_WPA_SCAN_SSID\"\n"
	"    fi;;\n"
	"  *) printf 'OK\\n';;\n"
	"esac\n";

static void write_fake_wpa_cli(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/wpa_cli", dir);
	FILE *f = fopen(path, "w");
	assert(f);
	assert(fputs(FAKE_WPA_CLI, f) >= 0);
	assert(fclose(f) == 0);
	assert(chmod(path, 0755) == 0);
}

static char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return strdup("");
	}
	static char buf[16384];
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void clear_log(void) {
	FILE *f = fopen(log_path, "w");
	assert(f);
	assert(fclose(f) == 0);
}

/* The line a call to wpa_cli left in the log, numbers from zero; NULL when
 * there is no such line. Lines are "-i|wlan0|set_network|0|scan_ssid|1|". */
static const char *log_line(const char *log, int wanted) {
	static char line[1024];
	const char *p = log;
	for (int i = 0;; i++) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (i == wanted) {
			if (len >= sizeof(line)) {
				len = sizeof(line) - 1;
			}
			memcpy(line, p, len);
			line[len] = '\0';
			return line;
		}
		if (!end) {
			return NULL;
		}
		p = end + 1;
	}
}

static int log_find(const char *log, const char *needle) {
	for (int i = 0;; i++) {
		const char *line = log_line(log, i);
		if (!line) {
			return -1;
		}
		if (strstr(line, needle)) {
			return i;
		}
	}
}

/* Waits for the worker to land the last request, and returns its verdict. */
static wifi_op_t wait_op(char *ssid_out, size_t ssid_size) {
	for (int i = 0; i < 1500; i++) {
		wifi_op_t op = wifi_take_op_result(ssid_out, (int)ssid_size);
		if (op == WIFI_OP_OK || op == WIFI_OP_FAILED) {
			return op;
		}
		struct timespec nap = {.tv_nsec = 10 * 1000 * 1000};
		nanosleep(&nap, NULL);
	}
	assert(!"the wifi worker never landed the request");
	return WIFI_OP_IDLE;
}

int main(void) {
	char dir[] = "/tmp/morpthis-wifi-test-XXXXXX";
	assert(mkdtemp(dir));

	snprintf(log_path, sizeof(log_path), "%s/wpa.log", dir);
	snprintf(saved_path, sizeof(saved_path), "%s/saved", dir);
	write_fake_wpa_cli(dir);

	char path_env[1024];
	snprintf(path_env, sizeof(path_env), "%s:%s", dir, getenv("PATH") ? getenv("PATH") : "");
	assert(setenv("PATH", path_env, 1) == 0);
	assert(setenv("FAKE_WPA_LOG", log_path, 1) == 0);
	assert(setenv("FAKE_WPA_SAVED", saved_path, 1) == 0);
	clear_log();

	wifi_init();

	char ssid[WIFI_SSID_MAX];

	/* A hidden network wpa_supplicant has never seen: created, told to probe
	 * for its name before any key is set or anything is selected, and only
	 * then joined. Without the middle step the rest is a network nothing
	 * will ever answer to. */
	wifi_connect_hidden("HiddenNet", "correct horse");
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	assert(strcmp(ssid, "HiddenNet") == 0);
	const char *log = slurp(log_path);
	int at_ssid = log_find(log, "set_network|0|ssid|\"HiddenNet\"|");
	int at_scan = log_find(log, "set_network|0|scan_ssid|1|");
	int at_psk = log_find(log, "set_network|0|psk|\"correct horse\"|");
	int at_select = log_find(log, "select_network|0|");
	assert(at_ssid >= 0 && at_scan > at_ssid && at_psk > at_scan && at_select > at_psk);
	assert(log_find(log, "add_network|") >= 0);

	/* A visible, open network: the ordinary join, and no scan_ssid anywhere
	 * in it -- a network that broadcasts its name is found by listening. */
	clear_log();
	wifi_connect("VisibleNet", NULL);
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	log = slurp(log_path);
	assert(log_find(log, "scan_ssid") < 0);
	assert(log_find(log, "set_network|0|key_mgmt|NONE|") >= 0);

	/* A hidden network wpa_supplicant already knows -- saved by the stock
	 * player, or by an earlier visit here. It is not added again, but its
	 * stored entry is not trusted to carry scan_ssid either, so the flag is
	 * written to the existing id before it is selected. */
	FILE *saved = fopen(saved_path, "w");
	assert(saved);
	assert(fclose(saved) == 0);
	clear_log();
	wifi_connect_hidden("SavedHidden", NULL);
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	log = slurp(log_path);
	assert(log_find(log, "add_network|") < 0);
	int known_scan = log_find(log, "set_network|0|scan_ssid|1|");
	int known_select = log_find(log, "select_network|0|");
	assert(known_scan >= 0 && known_select > known_scan);
	assert(unlink(saved_path) == 0);

	/* A saved network the scan reports as open: the access point's password
	 * was turned off since the entry was written, or the stock player wrote
	 * the entry with security of its own. The leftover key_mgmt is exactly
	 * what makes the supplicant scan past an access point that no longer
	 * encrypts -- not one authentication attempt, every join timing out --
	 * so joining it must rewrite the entry as open. */
	saved = fopen(saved_path, "w");
	assert(saved);
	assert(fclose(saved) == 0);
	assert(setenv("FAKE_WPA_SCAN_SSID", "SavedHidden", 1) == 0);
	assert(setenv("FAKE_WPA_SCAN_FLAGS", "[ESS]", 1) == 0);
	clear_log();
	wifi_connect("SavedHidden", NULL);
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	log = slurp(log_path);
	assert(log_find(log, "add_network|") < 0);
	int open_key = log_find(log, "set_network|0|key_mgmt|NONE|");
	int open_select = log_find(log, "select_network|0|");
	assert(open_key >= 0 && open_select > open_key);

	/* The same saved network, but the scan reports it secured: the stored
	 * key is being reused, and the entry must not be touched -- writing
	 * key_mgmt=NONE over it is how a saved network gets destroyed by
	 * tapping it. */
	assert(setenv("FAKE_WPA_SCAN_FLAGS", "[WPA2-PSK-CCMP][ESS]", 1) == 0);
	clear_log();
	wifi_connect("SavedHidden", NULL);
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	log = slurp(log_path);
	assert(log_find(log, "add_network|") < 0);
	assert(log_find(log, "set_network|0|key_mgmt|") < 0);
	assert(log_find(log, "select_network|0|") >= 0);

	/* And a saved network the scan does not list at all: out of range, its
	 * stored entry is everything there is to join by, and it stays. */
	assert(unsetenv("FAKE_WPA_SCAN_SSID") == 0);
	assert(unsetenv("FAKE_WPA_SCAN_FLAGS") == 0);
	clear_log();
	wifi_connect("SavedHidden", NULL);
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_OK);
	log = slurp(log_path);
	assert(log_find(log, "set_network|0|key_mgmt|") < 0);
	assert(log_find(log, "select_network|0|") >= 0);
	assert(unlink(saved_path) == 0);

	/* A passphrase wpa_supplicant will not store is refused up front, as a
	 * wrong passphrase, without the fake supplicant being told to store
	 * anything. (The status poll the worker runs behind every job is a read,
	 * not a word about this network, and may still be in flight here.) */
	clear_log();
	wifi_connect_hidden("HiddenNet", "short");
	assert(wait_op(ssid, sizeof(ssid)) == WIFI_OP_FAILED);
	assert(wifi_last_failure() == WIFI_FAIL_WRONG_KEY);
	struct timespec settle = {.tv_nsec = 300 * 1000 * 1000};
	nanosleep(&settle, NULL);
	log = slurp(log_path);
	assert(log_find(log, "add_network") < 0 && log_find(log, "set_network") < 0 && log_find(log, "select_network") < 0);

	printf("PASS wifi join (scan_ssid for hidden networks only; a saved network the scan "
		   "reports as open gets key_mgmt=NONE, a secured or out-of-range one keeps its entry)\n");
	return 0;
}
