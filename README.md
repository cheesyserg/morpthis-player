# Morpthis

![Morpthis for TempoTec Variations V1 with the Tokyo Night main menu](screenshots/readme-banner.png)

**Morpthis** is a community port and rebrand of the original **[Sonix Player](https://github.com/Jepl4r/sonix-player)** project by **[Jepl4r](https://github.com/Jepl4r)** — a replacement music player for the HiBy R3 Pro II and HiBy R1 — to the **TempoTec Variations V1**. The player and the firmware packer come from that project; this fork adds the V1 board profile, the V1 packaging path, the Morpthis identity and the V1 documentation. See [Credits](#credits).

The firmware builder starts with an official V1 update package, replaces the stock music player and its interface with Morpthis, then repackages the update as `v1.upt`.

This is not an official TempoTec or HiBy release. **The port has been tested on physical TempoTec Variations V1 hardware and works perfectly on the tested device.** It targets the **TempoTec Variations V1 only**; compatibility with the V1-A or other players in the Variations family is not established.


## Previews

Interface previews of the stock UI of Morpthis, rendered at the V1's native 240 × 320 resolution. Click any picture to open it full size. (This is not the default font neither the color)

<table>
  <tr>
    <td align="center">
      <a href="screenshots/main-menu.png"><img src="screenshots/main-menu.png" width="180" alt="Main menu"></a>
      <br><sub><b>Main menu</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/music.png"><img src="screenshots/music.png" width="180" alt="Music"></a>
      <br><sub><b>Music</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/more.png"><img src="screenshots/more.png" width="180" alt="More"></a>
      <br><sub><b>More</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/wireless.png"><img src="screenshots/wireless.png" width="180" alt="Wireless"></a>
      <br><sub><b>Wireless</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="screenshots/settings.png"><img src="screenshots/settings.png" width="180" alt="Settings"></a>
      <br><sub><b>Settings</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/now-playing.png"><img src="screenshots/now-playing.png" width="180" alt="Now Playing"></a>
      <br><sub><b>Now Playing</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/file-browser.png"><img src="screenshots/file-browser.png" width="180" alt="File Browser"></a>
      <br><sub><b>File Browser</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/streaming.png"><img src="screenshots/streaming.png" width="180" alt="Streaming"></a>
      <br><sub><b>Streaming</b></sub>
    </td>
  </tr>
</table>

## Contents

- [Target device](#target-device)
- [Features](#features)
- [Screenshots](#screenshots)
  - [Tokyo Night theme](#tokyo-night-theme)
  - [Ember theme](#ember-theme)
  - [OLED theme](#oled-theme)
- [Build a firmware image](#build-a-firmware-image)
- [Install or restore firmware](#install-or-restore-firmware)
- [Streaming credentials](#streaming-credentials)
- [Subsonic / Gonic](#subsonic--gonic)
- [Build from source](#build-from-source)
- [Project files and documentation](#project-files-and-documentation)
- [Credits](#credits)

## Target device

The **TempoTec Variations V1**:

| Component | Specification |
|---|---|
| **SoC** | Ingenic X1600 (MIPS32r2 core, o32 ABI, glibc 2.22 compatibility) |
| **Display** | 2.0-inch QVGA TFT panel, **240×320 pixels** portrait |
| **DAC** | Dual Cirrus Logic CS43131 |
| **Audio Outputs** | 3.5 mm single-ended + 4.4 mm balanced headphone jacks |
| **Storage** | One MicroSD card slot (exFAT / FAT32) |
| **Controls** | Side flank buttons (Power, Vol+, Vol-, Play/Pause, Next/Prev) + capacitive touchscreen |
| **Wireless** | 2.4 GHz Wi-Fi (802.11 b/g/n) and bi-directional Bluetooth |
| **USB** | USB Type-C (bidirectional USB DAC & USB audio out) |
| **Firmware Package** | `v1.upt` with accompanying `v1_md5.txt` |

The player UI is built with LVGL 9.6 and adapted for the V1's small display. The package is based on vendor firmware; it is **not** a Linux distribution or a kernel replacement.

## Features

The project includes a local music library and file browser, album artwork, playlists, lyrics, audiobooks and DSP controls. The player also contains code for streaming and network features such as Bluetooth audio, AirPlay, DLNA and Wi-Fi file transfer.

Local audio decoder inputs include WAV, FLAC, MP3, Ogg Vorbis, Opus, M4A/M4B/MP4, AAC, ALAC, WavPack, APE, AIFF/AIFC, CAF and DSD (DSF/DFF) files. DSP features include a 10-band graphic equalizer, parametric EQ, MSEB, crossfeed, channel balance and ReplayGain.

The interface font is a setting (Settings → Appearance → Font): MiSans, Neon 80s, Roboto, Roboto Condensed, Inter and Barlow Semi Condensed, plus any `.ttf`/`.otf` file put in the `Fonts` folder on the card. Whatever a face is missing — Cyrillic, Greek, Hangul, kanji, Arabic — is drawn by MiSans behind it, so the whole interface stays legible in every language. The four added families are under the SIL Open Font License; see [`sonix-player/assets/fonts/FONT-LICENSES.txt`](sonix-player/assets/fonts/FONT-LICENSES.txt) for the copyright notices and the exact files.

The boot screen is a setting too (Settings → Appearance → Boot screen), and it opens a **Boot screen centre**: a horizontal carousel of **Retrospace** (the default), **Stock** (the firmware's own light and dark pictures), and any JPEG or PNG the user puts in the `.bootscreen` folder at the root of the card. The picture in the middle is drawn large and the ones either side shrink as the row is dragged; the applied picture sits in its own column at the far left, behind a vertical rule, and **Apply** writes the selected one to the device. A custom picture is copied to the writable partition and into the same flash marker the theme uses, so it survives the card being removed and appears from the next power-on. The choice cannot live in the config alone — the script that draws the logo runs before any filesystem holding a config is mounted.

See [FEATURES.md](FEATURES.md) for a longer feature list. Streaming services also require valid credentials and may depend on third-party service availability.

## Install or restore firmware

Only attempt this on a TempoTec Variations V1. Read the warning above before proceeding.

1. Keep the **official V1 `v1.upt` and its matching `v1_md5.txt`** somewhere safe. They are needed if you want to return to stock.
2. Extract the workflow artifact — its name ends in the commit it was built from, and `BUILD-REPORT.txt` inside repeats it — and copy **both** `v1.upt` and `v1_md5.txt` to the root of a microSD card supported by the device. On a release, the uploaded `v1.upt` is always built from that release's own tag; if `BUILD-REPORT.txt` names another commit, the artifact is not the release's and should not be flashed as it.
3. Charge the player adequately, insert the card and start the device's built-in firmware update process. Menu names vary by stock firmware version; follow the device's on-screen instructions and do not interrupt the update.
4. If you want to restore stock firmware, put the original vendor `v1.upt` and its matching checksum file at the card root and use the same update process.

The package builder is designed to preserve the stock `xImage` kernel and leave the existing update/recovery path in place. This is checked during the build, but it is **not** a guarantee that recovery will work on every device or in every failure case.

## Streaming credentials

Credentials for Qobuz, Tidal, Podcast Index and Last.fm are not included in this repository or in the standard workflow artifact. Without usable credentials, those services remain disabled; local playback does not require them.

To try credentials on a build that has no keys embedded, copy `sonix-player/streaming-keys.ini.example` to the root of the microSD card as `streaming-keys.ini`, fill in only the services for which you have authorized credentials, then restart the player. Do not commit or share this file.

```ini
[qobuz]
app_id =
app_secret =

[tidal]
client_id =
client_secret =

[podcast]
api_key =
api_secret =

[lastfm]
api_key =
api_secret =
```

### Last.fm sign-in: TLS certificate error

If sign-in shows **“No reply from Last.fm”** and the player log says the certificate is not signed by a known authority, this is a TLS trust-store problem, not a Last.fm password or API-key problem. The player loads its CA bundle at startup.

On a Windows PC with ADB installed, connect the player by USB, enable **Developer options → ADB**, then run these commands in PowerShell. They download the current CA bundle from curl's official CA-bundle site and copy it to the microSD card:

```powershell
curl.exe -fL "https://curl.se/ca/cacert.pem" -o "$HOME\Downloads\cacert.pem"
Get-Item "$HOME\Downloads\cacert.pem"
adb shell mkdir -p /mnt/sd_0/.local
adb push "$HOME\Downloads\cacert.pem" /mnt/sd_0/.local/cacert.pem
adb shell ls -l /mnt/sd_0/.local/cacert.pem
```

`Get-Item` should show that the downloaded file exists and has a non-zero length; the final `ls` checks the copy on the card. If the file was downloaded somewhere other than `Downloads`, use its actual path in the `adb push` command.

If `[network] ca_bundle` is set in `/usr/data/device_config.ini`, that configured file takes priority: update that file instead. Otherwise, the player checks `/mnt/sd_0/.local/cacert.pem` before its firmware CA bundle. After copying, restart the player normally so it reloads the certificates; do not interrupt an update that is still in progress. If the TLS error persists, try a different trusted Wi-Fi network and see [how to collect a log](docs/collecting-logs.md). Never share `streaming-keys.ini`, API secrets, or your Last.fm password.

### Your own server with a self-signed certificate

A home server with a self-signed certificate — the usual setup for Subsonic/Gonic, and something no CA bundle can ever validate — is refused by default. **Accept self-signed certificates** under **Streaming → Subsonic → Server settings** lets it through, without `adb` and without a restart. The same thing can be set in `/usr/data/device_config.ini`:

```ini
[network]
tls_verify = relaxed
```

**Relaxed** forgives a certificate whose issuer is not in the trust store, and nothing else: the certificate still has to name the server being contacted, and its dates still have to cover the current date, so an expired certificate or one issued to a different name fails and says so. A certificate error that names a date usually means the player's clock is wrong, not that the mode is missing.

The setting is global — it applies to every `https://` connection the player opens, radio stations included — because one TLS context serves them all. Anyone using a public network should leave it at the default, `strict`. A private certificate authority is better handled with `[network] ca_bundle` above, which keeps checking on: export the CA's own certificate, copy that file to the card, and point `ca_bundle` at it.

For a private development build, `sonix-player/tools/seal_streamkeys.py` can place a sealed credentials file in the assets before compilation. This format is **obfuscation, not secure encryption**: the key is compiled into the player binary and can be recovered by someone who analyzes it. Never publish a binary containing credentials you do not intend to disclose.

## Subsonic / Gonic

**Streaming → Subsonic → Server settings** connects to your own Subsonic-compatible server, including Gonic. Enter its base URL and your account, then select **Connect / save**. Browse Music folders or Artists and select a track to play the current page.

MP3 at **256 kbps** is the default; **320 kbps**, **Opus**, and **Raw** (original FLAC/MP3/Opus) are available. Transcoding must be enabled on the server. Network workers feed two fixed **1 MiB RAM rings**, refill at about 50%, and pre-cache the next track's first **512 KiB** near 90% progress. There is no disk audio cache, localhost proxy, or socket read in the decoder.

No application API key is needed. Prefer HTTPS; credentials stay in memory unless explicitly remembered, and remembered passwords are stored **unencrypted** in a private device-local file. A server with a self-signed certificate needs **Accept self-signed certificates** on its settings page — see [your own server with a self-signed certificate](#your-own-server-with-a-self-signed-certificate). Seeking and reboot restoration are not available for these ephemeral streams. Compatible-format gapless handoff uses the existing ALSA path; network, codec timing and physical-device limits still apply.

See [setup, design, limitations and the V1 validation checklist](docs/subsonic.md), and [host integration tests](sonix-player/tests/README.md).

## Project files and documentation

- `morpthis-player/` — player sources, LVGL configuration, launcher and build system.
- `morpthis-packer/` — stock-image preparation, firmware packaging and validation scripts.
- `docs/collecting-logs.md` — which logs to send with a bug report, and how to take them.
- `docs/subsonic.md` — Subsonic/Gonic setup, RAM transport and hardware validation.
- `docs/sgm31324-led-control.md` — driving the RGB status LED directly over I²C.
- `docs/apps.md` — the on-card app contract: layout, manifest, launch model and limits.
- `docs/app-sdk.md` — the SDK repository: toolchain, ABI, API surface and packaging. The player build needs its public headers: clone `morpthis-sdk` beside this repository or set `MORPTHIS_SDK`.
- `docs/apps-repo.md` — the apps repository: structure, CI, releases and contribution rules.
- `morpthis-player/tools/morpthis-bugreport.sh` — one-file diagnostic bundle, run on the device over ADB.
- `FEATURES.md` — detailed feature notes.
- `PATCHES.md` — device-porting and implementation notes.
- `LICENSE` — GNU General Public License, version 3. Third-party components retain their own licenses.

For bug reports, include the exact model, the firmware build/commit and relevant logs — see [Collecting logs for a bug report](docs/collecting-logs.md) for what to turn on and where the files are. Remove personal data and **never attach streaming credentials**.

## Credits

This port exists because of the original **[Sonix Player](https://github.com/Jepl4r/sonix-player)** project. Almost everything in `sonix-player/` and `sonix-packer/` is that project's work, carried here and adapted; what this fork adds is the TempoTec V1 support and the documentation for it. The original is licensed under the GPL-3.0, and so is this fork.

- **[Jepl4r](https://github.com/Jepl4r)** — author and maintainer of Sonix Player, and the source of the player, the packer and the interface this port is built on.
- **[Tartarus6](https://github.com/Tartarus6)**, **[noisetta](https://github.com/noisetta)**, **[endgame47](https://github.com/endgame47)**, **[hkhrithik007](https://github.com/hkhrithik007)** — the people named in the original project's own "Special thanks" list.
- **[Hinatai](https://github.com/Hinatai)**, **[KrajzegaX](https://github.com/KrajzegaX)**, **[noemdespertis](https://github.com/noemdespertis)** and everyone else on the upstream [contributor list](https://github.com/Jepl4r/sonix-player/graphs/contributors), whose commits this fork inherits.
- The HiBy and TempoTec communities — the people who test, report, translate and explain both projects — who do the work no commit log shows.

Third-party components carried in the tree, or fetched by the build, keep their own authors and licences:

| component | used for |
|---|---|
| [LVGL](https://github.com/lvgl/lvgl) 9.6 | the whole interface |
| [dr_libs](https://github.com/mackron/dr_libs) by David Reid | FLAC and MP3 decoding |
| [stb](https://github.com/nothings/stb) by Sean Barrett | cover art and image read/write |
| [miniz](https://github.com/richgel999/miniz) (RAD Game Tools, Valve, Rich Geldreich) | inflate for covers |
| TJpgDec by ChaN | JPEG decoding on the target |
| [SQLite](https://sqlite.org) | the library index |
| [FreeType](https://freetype.org) | fonts at runtime |
| [libogg, libopus and opusfile](https://opus-codec.org) (Xiph.Org) | Opus playback |
| [WavPack](https://www.wavpack.com) by David Bryant | WavPack playback |
| [BlueALSA](https://github.com/arkq/bluez-alsa) by Arkadiusz Bokowy | Bluetooth audio in the HiBy overlay trees, inherited from upstream |
| [Lucide](https://lucide.dev) | the Wi-Fi transfer page glyphs |
| the [Rockbox](https://rockbox.org) toolchain scripts in `sonix-player/rockboxdev/` | the MIPS cross-compiler |

The licence notice for each component sits with it in the source tree; [LICENSE](LICENSE) covers the project as a whole. The official TempoTec V1 firmware used as the base of a build is not part of this repository, and TempoTec, HiBy and the other vendors own their names and firmware.

