# AirPlay: shairport-sync classic, pipe backend

Status: **integration code written, not yet built or tested on device.** The
player keeps the stock shairport path behind the same switch, so an image
without the new binary behaves exactly as it did before.

## Decision

Replace the stock `/usr/bin/shairport` (ancient abrasive/shairport with a
private `ot` unix-socket backend) with **shairport-sync 5.x, classic AirPlay
only**, built with:

| Component | Choice | Why |
| --- | --- | --- |
| AirPlay | Classic (AirPlay 1) | AirPlay 2 needs NQPTP, FFmpeg, libplist, libsodium, libgcrypt and 48 kHz buffered AAC. Not viable on 64 MB / one core, and not needed for phone-to-DAP playback. |
| Audio output | `--with-pipe` | The player already owns the DAC through `audio_external_*()`, the ring, route switching, power holds and hand-back. shairport-sync writes raw S16_LE 44100 stereo into a FIFO and the player paces it with blocking reads, exactly as the old socket did. |
| mDNS | `--with-tinysvcmdns` | No Avahi daemon exists on the device. |
| TLS | `--with-ssl=openssl` | Device ships OpenSSL 1.1.1; static linking avoids an ABI/patch-level dependency. |
| Metadata | `--with-metadata --with-metadata-pipe` | Title/artist/album/cover over XML. |
| Decoder | `--with-hammerton` | Bundled `alac.c`, no extra library. Deprecated upstream for security; acceptable for streams from a phone on the local network. Swap to Apple ALAC + libalac if that changes. |
| Not used | soxr, FFmpeg, ALSA, Avahi, D-Bus, MPRIS, MQTT, systemd | Each one is binary size, RAM or a dependency the device does not have. |

The device's AIC driver takes S16_LE and S24_LE and nothing else, and the
player already avoids the S32→S24 plug/mmap path that has crashed this kernel.
The pipe backend's classic default is **S16_LE / 44100 / 2**, so no conversion
happens anywhere between the phone and the DAC.

## Data flow

```
phone ──RAOP/TCP──▶ shairport-sync (classic, ALAC→S16_LE)
                          │
                          ├─ /tmp/shairport-sync-audio      FIFO, blocking writes
                          │        │
                          │        ▼
                          │   airplay.c fifo_worker → ring → audio_external_write()
                          │                                      │
                          │                                      ▼
                          │                                   DAC / ALSA
                          │
                          └─ /tmp/shairport-sync-metadata   FIFO, XML items
                                   │
                                   ▼
                             meta XML parser → state.title/artist/album/genre
                                             → pbeg/pend/pfls/abeg/aend
```

The player's existing ring, 190 ms buffer, silence hand-back, route following,
LED and power handling are unchanged.

### Known tradeoff: the pipe backend has no rate control

shairport-sync's ALSA backend adjusts the DAC rate to follow the sender's clock.
The pipe backend does not (`audio_pipe.c`: "No interpolation is done"): it
writes, the player's ring drains at the DAC's own clock, and any difference
between the phone and the DAP shows up as the pipe slowly filling or emptying.
The 64 KB kernel pipe plus the 32 KB ring absorb roughly half a second, which
covers clock drift over an album-length session. A very long continuous stream
may eventually block the writer (latency grows, then stays bounded) or leave a
gap. If that proves audible, the options are a gentle one-frame insert/drop in
the player when the ring sits at an edge, or direct ALSA with the exclusivity
that implies. Measure before adding either.

## Backend selection

`airplay.c` picks the backend at service start:

1. `[airplay] backend = sync` forces shairport-sync.
2. `[airplay] backend = stock` forces the old `ot` path.
3. Unset (default): shairport-sync if `/usr/bin/shairport-sync` exists,
   otherwise the stock daemon.

`airplay_available()` follows the same choice, so the AirPlay page appears only
when the selected daemon and both scripts are executable.

## Firmware files

Added to the packer overlay (`morpthis-packer/assets/V1/`):

| Path | Purpose |
| --- | --- |
| `/etc/shairport-sync.conf` | pipe backend, S16_LE/44100/2, `ignore_volume_control = "yes"`, metadata pipe |
| `/usr/bin/shairport_on.sh` | starts shairport-sync; falls back to the stock command if the binary is absent |
| `/usr/bin/shairport_off.sh` | `killall shairport-sync`, then `killall shairport` |

`morpthis_firmware_packer.sh` installs `shairport-sync` from next to
`morpthis_player` when it is present. Nothing else changes: the player still
asks `sys_server` for `SHAIRPORT:TURN_ON:<name>` / `TURN_OFF`, and the scripts
are the only place the daemon name appears.

The advertised name still travels with spaces as `%`, because `sys_server`
pastes the argument into a shell command unquoted. `shairport_on.sh` turns them
back into spaces with `sed` before passing `-a`.

## Build

```sh
./tools/build-shairport-sync.sh
```

It fetches popt 1.19, libconfig 1.7.3 and OpenSSL 1.1.1w on first use, builds
them statically into `tools/airplay-target/prefix`, configures shairport-sync
from the sibling checkout (`../../shairport-sync`), and writes a stripped
binary to `morpthis-packer/shairport-sync`.

Requirements on the build machine: the pinned rockbox toolchain (the same one
the player uses), `autoconf`/`automake`/`libtool`, `pkg-config`, `curl` or
`wget`, and a network connection for the first run. `CROSS_CC`, `SHAIRPORT_SRC`
and `PACKER_DIR` can be overridden.

The script cannot be validated in the current container (no cross toolchain,
no network). The first build is the next step.

## Phases

- [x] Player: backend switch, FIFO audio reader, shairport-sync XML metadata parser.
- [x] Firmware: config file, start/stop scripts, packer install step.
- [x] Build recipe written (`tools/build-shairport-sync.sh`).
- [ ] Run the build, confirm mipsel/static, and boot with the new binary.
- [ ] Device: iPhone/Mac playback, pause/resume, track change, volume, metadata.
- [ ] Device: measure RSS (`MemAvailable` before/after) and CPU while playing.
- [ ] Device: standby with AirPlay on but idle (DAC must hand back after 5 s).
- [ ] Device: switching AirPlay off returns local playback and apps.
- [ ] Device: long-session drift (run an album, then two hours; watch for a
growing ring or gaps) and decide whether the player needs a rate corrector.
- [ ] Cover art: save `PICT` to a file and feed `cover_load_image_file()`.
- [ ] Remove the stock path once shairport-sync has been stable for a release.

## Offline "no-gateway" access point

`tools/airplay-ap-probe.sh` is the first step: it changes nothing and reports
whether the driver advertises AP mode. Do not build hostapd support before
that answer is `* AP`.

The car guide's premise is that the phone keeps cellular for the internet and
uses a local Wi-Fi link only for AirPlay. On this DAP that means the player
would run an access point instead of joining one. **This is only possible if
the Wi-Fi driver and firmware support AP mode; nothing in the current image
starts hostapd.**

Probe on the device first (the script is in the player tree):

```sh
adb push tools/airplay-ap-probe.sh /tmp/
adb shell sh /tmp/airplay-ap-probe.sh
```

It reports the driver's supported interface modes, the supplicant's
capabilities, and whether hostapd and a DHCP server are already present.
`* AP` under "Supported interface modes" is the go-ahead. The same commands by
hand:

```sh
iw list | sed -n '/Supported interface modes/,/^[^ \t]/p'
wpa_cli -i wlan0 get_capability modes
ls -l /usr/bin/hostapd /usr/sbin/hostapd
busybox --list | grep -E 'udhcpd|dnsmasq'
```

Proceed only if `* AP` is listed. Then:

- Cross-build `hostapd` (needs libnl and OpenSSL) and ship it, or use an
  existing binary if the stock image carries one.
- `/etc/hostapd-airplay.conf`: `driver=nl80211`, `hw_mode=g`, a fixed channel
  (6), `wmm_enabled=1`, WPA2-PSK with a passphrase from the config.
- DHCP without a gateway: BusyBox `udhcpd` with `start 192.168.4.10`,
  `end 192.168.4.50`, `option subnet 255.255.255.0`, and **no `option router`,
  no `option dns`**. The phone then has a local address but no default route
  through the DAP, so iOS keeps cellular for internet and uses the link for
  AirPlay.
- Exclusivity: stop `wpa_supplicant` before `hostapd`, and restore the station
  connection when the hotspot goes off. While it is on, Wi-Fi transfer, DLNA,
  OTA and internet-dependent apps are unavailable.
- Power: add `airplay_hotspot_active()` to `wifi_in_use()` in `power.c` so
  standby does not park the radio. The screen may still go dark.
- UI: a toggle on the AirPlay page, persisted as `[airplay] hotspot`, shown
  only when the probe says the mode is supported. Never silently switch a
  connected station off.

This is a separate phase. It is not needed for AirPlay from a phone on the same
network as the DAP.
