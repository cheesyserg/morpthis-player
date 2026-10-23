#!/bin/sh
#
# Run this ON the player (adb push + adb shell sh /tmp/airplay-ap-probe.sh) to
# find out whether the Wi-Fi hardware can host the AirPlay access point.
#
# Nothing here changes the system. It only reports what the driver, the
# supplicant and BusyBox can do.

IFACE=wlan0

echo "== wireless interfaces"
for net in /sys/class/net/*; do
	[ -e "$net/wireless" ] || continue
	echo "   $(basename "$net")"
done

echo
echo "== driver interface modes (iw)"
if command -v iw >/dev/null 2>&1; then
	iw list 2>/dev/null | sed -n '/Supported interface modes/,/^[^ \t]/p'
else
	echo "   iw not installed"
fi

echo
echo "== supplicant capabilities"
if command -v wpa_cli >/dev/null 2>&1; then
	echo -n "   modes: "
	wpa_cli -i "$IFACE" get_capability modes 2>/dev/null || echo "(no answer)"
	echo -n "   current mode: "
	wpa_cli -i "$IFACE" status 2>/dev/null | sed -n 's/^mode=//p'
else
	echo "   wpa_cli not installed"
fi

echo
echo "== access point daemon"
for p in /usr/bin/hostapd /usr/sbin/hostapd /sbin/hostapd; do
	[ -x "$p" ] && echo "   $p"
done
command -v hostapd >/dev/null 2>&1 || echo "   hostapd not on PATH"

echo
echo "== DHCP server"
if command -v busybox >/dev/null 2>&1; then
	busybox --list 2>/dev/null | grep -E '^(udhcpd|dnsmasq)$' | sed 's/^/   /'
fi
for p in /usr/bin/dnsmasq /usr/sbin/dnsmasq; do
	[ -x "$p" ] && echo "   $p"
done

echo
echo "== kernel modules that look like they can do AP mode"
lsmod 2>/dev/null | grep -Ei '8189|8188|8723|8821|brcm|bcmdhd|ath|rtl|wlan' | sed 's/^/   /' || true

echo
echo "Verdict: if 'Supported interface modes' lists '* AP', the driver can do it."
echo "hostapd and a DHCP server still have to be cross-built and shipped."
