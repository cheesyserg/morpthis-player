#!/bin/sh
# sonix-bugreport.sh - one-file diagnostic bundle for the Sonix Player.
#
# Run it ON the player, from a shell over ADB:
#
#     adb push sonix-player/tools/sonix-bugreport.sh /tmp/
#     adb shell sh /tmp/sonix-bugreport.sh
#     adb pull /mnt/sd_0/.local/bugreport-*.txt
#
# It writes a single text file - beside the player's own log on the card, or in
# /tmp when there is no card - and prints the path it picked. Everything leaves
# the script redacted: passwords, PSKs, tokens, cookies, logins and API keys are
# replaced with <redacted>, so the file can be attached to an issue as it is.
# Read it once before posting anyway.
#
# Knobs, set in the environment:
#     adb shell "SONIX_LOG_LINES=20000 sh /tmp/sonix-bugreport.sh"
#     adb shell "SONIX_FULL_LOG=1 sh /tmp/sonix-bugreport.sh"
#
#   SONIX_LOG_LINES   lines kept from the tail of each log  (default 4000)
#   SONIX_FULL_LOG=1  copy whole logs instead of their tails
#   SONIX_OUT_DIR=dir write the bundle somewhere else
#
# Plain POSIX sh: the player's firmware has busybox, not bash.

set -u

CARD=""
for c in /mnt/sd_0 /data/mnt/sd_0 /usr/data/mnt/sd_0; do
	if [ -d "$c" ]; then
		CARD="$c"
		break
	fi
done

OUT_DIR="${SONIX_OUT_DIR:-}"
if [ -z "$OUT_DIR" ]; then
	OUT_DIR="$CARD/.local"
	[ -n "$CARD" ] || OUT_DIR=/tmp
fi
[ -d "$OUT_DIR" ] || mkdir -p "$OUT_DIR" 2>/dev/null || OUT_DIR=/tmp

STAMP=$(date +%Y%m%d-%H%M%S 2>/dev/null)
[ -n "$STAMP" ] || STAMP="nodate"
OUT="$OUT_DIR/bugreport-$STAMP.txt"
LOG_LINES="${SONIX_LOG_LINES:-4000}"

echo "sonix-bugreport: writing $OUT" >&2
exec >"$OUT" 2>&1

PID=$(pidof sonix_player 2>/dev/null)

# ---------------------------------------------------------------- redaction

# Replace the value of any line that assigns to a secret-sounding name. The
# player keeps Wi-Fi PSKs in wpa_supplicant, Last.fm sessions and streaming
# logins in /usr/data/device_config.ini, and the log can print a URL with a
# token in it, so this runs over every file the bundle copies.
redact() {
	awk '
	BEGIN {
		n = split("password passwd pass pwd psk passphrase token secret " \
		          "api_key apikey api_secret client_secret authorization " \
		          "cookie session_key session refresh_token refresh access_key " \
		          "login username user email", k, " ")
	}
	{
		low = tolower($0)
		hit = 0
		for (i = 1; i <= n && !hit; i++) {
			p = index(low, k[i])
			if (p == 0) continue
			rest = substr($0, p + length(k[i]))
			if (rest ~ /^[ \t]*[=:]/) {
				match(rest, /^[ \t]*[=:][ \t]*/)
				print substr($0, 1, p + length(k[i]) - 1) substr(rest, 1, RLENGTH) "<redacted>"
				hit = 1
			}
		}
		if (!hit) print
	}'
}

section() { printf '\n===================== %s =====================\n' "$*"; }

# Runs a command and keeps the head of whatever it says, so one runaway
# command cannot make the bundle useless.
run() {
	section "$1"
	sh -c "$1" 2>&1 | head -n "${2:-200}" | redact
}

# The end of a file (all of it with SONIX_FULL_LOG=1), redacted, with its size
# and mtime in front so a stale log is obvious.
tailfile() {
	if [ ! -f "$1" ]; then
		section "$1"
		echo "(no such file)"
		return
	fi
	section "$1"
	ls -l "$1" 2>&1
	if [ "${SONIX_FULL_LOG:-0}" = "1" ]; then
		redact <"$1"
	else
		tail -n "$LOG_LINES" "$1" | redact
	fi
}

# ---------------------------------------------------------------- the bundle

echo "sonix-player bug report bundle"
echo "written:   $(date 2>/dev/null)"
echo "bundle:    $OUT"
echo "card:      ${CARD:-<none mounted>}"
echo "pid:       ${PID:-<sonix_player not running>}"
echo
echo "This file is redacted (passwords, PSKs, tokens, logins -> <redacted>)."
echo "Fill in the last section before attaching it to an issue."

section "device and build"
run 'cat /usr/resource/sonix/components/system-info.json'
run 'ls -l /usr/bin/sonix_player /usr/bin/sonix_launch'
run 'md5sum /usr/bin/sonix_player'
run 'cat /etc/version'
run 'cat /etc/os-release'

section "runtime"
run 'date'
run 'cat /proc/uptime'
run 'uname -a'
run 'cat /proc/version'
run 'cat /proc/cmdline'
run 'cat /proc/loadavg'
run 'lsmod'

section "memory"
run 'cat /proc/meminfo' 60
run 'free'
run 'ps -o pid,ppid,stat,vsz,rss,comm'
run 'top -b -n 1' 60
if [ -n "$PID" ]; then
	run "cat /proc/$PID/status" 60
	run "ls /proc/$PID/task | wc -l"
	run "cat /proc/$PID/wchan"
	run "cat /proc/$PID/stack" 40
fi

section "kernel log"
run 'dmesg' 800
run 'cat /proc/last_kmsg' 200
run 'ls -l /sys/fs/pstore'
for f in /sys/fs/pstore/*; do
	[ -f "$f" ] || continue
	run "cat '$f'" 200
done

section "storage"
run 'mount'
run 'df -h'
if [ -n "$CARD" ]; then
	run "ls -l '$CARD'"
	run "ls -la '$CARD/.local'"
	run "ls -l '$CARD/Screenshots'" 60
fi

section "player log"
tailfile "${CARD:-/tmp}/.local/sonix_player.log"
run 'ls -la /tmp'

section "bluetooth log"
tailfile "${CARD:-/tmp}/.local/bluetooth.log"
if [ "${SONIX_FULL_LOG:-0}" = "1" ]; then
	tailfile "${CARD:-/tmp}/.local/bluetooth.old.log"
fi

section "boot trace (boot screen / logo)"
run 'cat /tmp/.bootlogo_trace'
run 'cat /tmp/sonix-log-early' 100

section "settings (redacted)"
run 'ls -l /usr/data'
if [ -f /usr/data/device_config.ini ]; then
	section "/usr/data/device_config.ini"
	ls -l /usr/data/device_config.ini
	redact </usr/data/device_config.ini
fi

section "network"
run 'ip link show'
run 'ifconfig -a'
run 'iwconfig'
run 'wpa_cli -i wlan0 status'
run 'wpa_cli -i wlan0 list_networks'
run 'route -n'
run 'cat /etc/resolv.conf'

section "audio"
run 'cat /proc/asound/cards'
run 'cat /proc/asound/pcm'
run 'cat /proc/asound/devices'
run 'amixer -c 0 scontents' 200
run 'ps -o pid,stat,comm | grep -E "bluealsa|bluetoothd|mpd|adbd" '

section "display and input"
run 'ls -l /dev/fb* /dev/input'
run 'cat /sys/class/graphics/fb0/virtual_size'
run 'cat /sys/class/graphics/fb0/modes'
run 'fbset'
run 'cat /proc/bus/input/devices'

section "usb"
run 'ls /sys/class/udc'
run 'ls /sys/kernel/config/usb_gadget'
run 'cat /sys/kernel/config/usb_gadget/*/UDC'

section "what you were doing"
cat <<'EOF'

Fill this in before attaching the file:

  What happened:
  What you expected:
  Steps that reproduce it (numbered, from a cold boot if that matters):
  How often (every time / 1 in 5 / once):
  Time it happened (the log is stamped with the device clock):
  Firmware build (Settings > System > About, or system-info.json above):
EOF

echo
echo "end of bundle: $OUT"
