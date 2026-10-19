# Collecting logs for a bug report

Everything below is about the player as it runs on the device. Paths are the
paths on the device; the card is the microSD, mounted at `/mnt/sd_0`.

## The short version

1. **Settings → System → About**, tap **Build number** five times, until it says
   developer options are on.
2. **Settings → Developer options → Log to microSD** → on.
3. Reproduce the bug.
4. Take **`.local/morpthis_player.log`** from the root of the card. `.local` is a
   hidden folder, so allow hidden files in your file manager (or `adb pull
   /mnt/sd_0/.local/morpthis_player.log`).

That one file is the answer to "what log do I send" in almost every case. Every
line is stamped with the time to the millisecond, a date line is printed at the
start and whenever the day changes, the kernel's warnings and errors are copied
into it as `kernel:` lines about once a second, and a crash prints its own
report into it.

Five things about it that decide whether it is useful:

- **It only starts when you turn it on.** Lines from before you switched it on
  do not exist. Turn it on first, *then* reproduce.
- **It is never truncated or rotated.** It is opened in append mode and grows
  for as long as the switch is on. Delete it (or rename it) right before
  reproducing, so the file you send is the bug and not last week.
- **It is flushed to the card about once a second.** Wait two or three seconds
  after the bug before pulling the card or switching to USB mass storage — the
  writer thread syncs the file every second, so only the last second is ever at
  risk.
- **A crash or a reboot is reported in the *next* run's log, not the previous
  one.** At every start the player reads what the kernel kept from before —
  out-of-memory kills, oopses, pstore records, `/proc/last_kmsg` — and writes it
  into the top of the new log as `kernel:` lines. If the device restarts, send
  the log as it is *after* it has come back, and say so.
- **While the card is exported over USB the log is held in memory** and appended
  afterwards, so the export window itself is missing from the file. That is
  expected, not a lost log.

Turn the switch off when you are done: it is a constant trickle of writes to
your music card.

## When the log is not enough: the bundle

For anything that is not visible from the player alone — a freeze, a reboot, a
kernel or driver problem, Wi-Fi, Bluetooth, touch, audio — the useful thing is a
bundle: the log plus the state of the device at the moment you took it.

Enable **Developer options → ADB**, connect the player over USB, then from a
computer with `adb`:

```sh
adb push sonix-player/tools/morpthis-bugreport.sh /tmp/
adb shell sh /tmp/morpthis-bugreport.sh
adb pull /mnt/sd_0/.local/bugreport-*.txt
```

It writes one file beside the player's log (`/mnt/sd_0/.local/bugreport-<date
-time>.txt`, or `/tmp/` when there is no card) and prints the path. It collects:
device and build, kernel log and pstore, memory and processes, mounts and card
state, the player log, the Bluetooth log, the boot trace, the settings, network,
audio, display and input, USB state. Passwords, PSKs, tokens, cookies and logins
are replaced with `<redacted>` on the way out; the file ends with a short form to
fill in (what happened, expected, numbered steps, time, build).

Two knobs, set in the environment:

```sh
adb shell "MORPTHIS_LOG_LINES=20000 sh /tmp/morpthis-bugreport.sh"   # longer tails (default 4000)
adb shell "MORPTHIS_FULL_LOG=1     sh /tmp/morpthis-bugreport.sh"    # whole logs, not tails
```

## Symptom → what else to send

| Symptom | Add |
|---|---|
| Bluetooth: pairing, connecting, drops, no audio | **Developer options → Bluetooth log** on, reproduce, send `.local/bluetooth.log` (and `.local/bluetooth.old.log` if there was a lot of it — it rotates at 8 MB). It has btmon's HCI traffic, `bluetoothd`/`bluealsa` syslog and the player's own `bluetooth:` lines on one timeline. Also the device you connect *to*: phone or headphones, make and model. |
| Crash during a library scan | **Developer options → Database log** on (under the log switch). The scan then names every file it reads, so the log ends at the file that caused it. Give the file if you can. |
| Player restarts, device reboots, hard freeze | The log *after* the restart (see above), plus a bundle for `dmesg` and pstore. A SIGKILL from the out-of-memory killer leaves nothing behind in the player's own output — only the kernel's buffer has it. |
| Wrong or missing boot screen | **Developer options** shows the boot trace in its own row; the bundle includes `/tmp/.bootlogo_trace`. Say which boot screen is selected and what you got instead. |
| Layout, fonts, missing glyphs, colors | Screenshots: **volume up + power** saves to `Screenshots/` on the card. The bundle's display and input sections cover the panel size and rotation. |
| Touch: dead zones, ghost taps, scrolling | The bundle's input section (`/proc/bus/input/devices`, `/dev/input`). For a stock-vs-patched driver question, see [PATCHES.md](../PATCHES.md). |
| Wi-Fi, DLNA, AirPlay, Wi-Fi file transfer | The bundle's network section. It prints `wpa_cli status` and the network list — no PSKs. Say which access point band and what the other end is. |
| Audio glitches, wrong output, DAC | The bundle's audio section (`/proc/asound`, mixer). Add the Bluetooth log if it happens over Bluetooth. |
| Battery, charging, standby, power-off | The player log around the event, with the time you plugged or unplugged it. |

## Always write down, next to the log

- Numbered steps that reproduce it, starting from a cold boot if that matters.
- How often: every time, one in five, once.
- **When** it happened, as the device's own clock had it. The log is stamped
  with local time, and the device's clock is not your phone's clock; a time lets
  the log be read even when it is days long.
- The **firmware build** (Settings → System → About, or
  `/usr/resource/morpthis/components/system-info.json`), and the exact model.
- What is on the card: filesystem (exFAT/FAT32), make, and roughly how many
  tracks. Whether it still happens with no card inserted, if you can try.

## Before you attach anything

- Never attach `streaming-keys.ini`, or a build made with your own credentials.
- The bundle redacts, but read it once: a file name, an SSID or a playlist title
  can still be personal. Rename or cut whatever you would not post.
- Library logs name the files on your card.
- If the file is big, gzip it. Bigger is better than truncated — a tail cut in
  the wrong place removes the lines that matter.
