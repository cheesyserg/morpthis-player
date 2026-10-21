# Subsonic / stream-bridge integration tests

Run from the repository root on a Linux host:

```sh
make -C sonix-player/tests test
make -C sonix-player/tests sanitize
make -C sonix-player/tests swvolume
```

These require only a C compiler, Make and Python 3 (plus standard pthread/dl/math libraries). They **do not require LVGL, ALSA, a MIPS toolchain, a music server, a real account, or downloaded audio**.

`run_subsonic_tests.py` starts a temporary threaded localhost HTTP server plus three HTTPS servers carrying the certificates in `tls_fixtures.py`, runs the C binary with a private temporary config directory, verifies its requests, then shuts down. All account values are synthetic. Build products live in ignored `.build/`; response/PCM fixtures are generated in memory rather than added to Git, and the three test certificates are pasted into that Python file rather than downloaded.

## Coverage

The C binary links the **real** HTTP/TLS helpers, REST client, MD5, JSON, configuration, playlist, RAM producer/reader and two-slot manager. `support.c` replaces only unrelated platform scheduling/signal helpers, translations and local-library indexes; it does not replace networking, buffers or decoding.

Checks include:

- correct salted MD5 authentication with encoded UTF-8/special characters, fresh salts, and no password/token query in diagnostic output;
- TLS against real self-signed certificates: `strict` (the default) refuses one, `relaxed` accepts it, a certificate naming another server and an expired one are refused either way, a connection opened under the old mode survives the setting changing under it, and the mode round-trips through `device_config.ini`;
- server URL/port rejection, exact opaque/numeric IDs (including >64-bit IDs), bounded JSON/token parsing, and malformed/oversized/auth-failed responses;
- music folders → indexes → directory and artists → artist → album, and 200-entry pagination;
- optional atomic 0600 credential persistence/round-trip, clearing remembered credentials, reporting a failed Forget instead of false success, rejecting symlinks/truncated identities/embedded NUL;
- fixed RAM capacity, millions of bytes across ring wrap, 50% refill hysteresis, protected initial header read/rewind vs a racing producer, retained-header rewind vs overwritten history, and 512 KiB pre-cache promotion;
- chunk extensions/trailers, transfer framing never reaching the reader, truncation/empty bodies/error pages, and retry after timeouts inside a chunk size, CRLF and trailer;
- cancellation during HTTP header setup, a stalled body, and reader starvation, followed by generation-safe reuse;
- nonmutating playlist next-track look-ahead across repeat/shuffle/ordered modes;
- exclusive decoder leases, both handoff/teardown orders, failed pre-cache retry suppression, repeat-one starting at byte zero, account-change invalidation with an outstanding lease, metadata lookup and unsupported raw codecs.

## Package mount registry

`make -C sonix-player/tests pkgmerp` links the real `pkgmerp.c` and wraps only
the loop and mount syscalls. It checks that the first package mounts (the free
slot is not looked up by its mount-point path), six sequential mounts release
their slots, four simultaneous mounts fit and the fifth is refused, and a
shared mount survives until the last reference is dropped. No loop devices,
root or card are needed. It is part of `make test`.

## Software volume

`make -C sonix-player/tests swvolume` checks the arithmetic of the software
volume -- the whole level law for a USB DAC with no control of its own. The
gain must mean exactly the decibels the USB route asked for, mute must be
exact, a deeply attenuated stream must stay quiet, clean and in range (rounded
and dithered, not truncated into grit), and the scaling must apply only while
the sound is leaving over the USB-C port. The three routing answers (Bluetooth,
a card on the port, that card's volume control) are stubbed; the curve, the
coefficients and the scaling are the real ones.

## Wi-Fi hidden networks

`make -C sonix-player/tests wifi` checks the one command that makes a hidden
network joinable. It links the real `wifi.c` and drives it against a fake
`wpa_cli` shell script placed first on `PATH` (the worker reaches it through
`execvp`, exactly as on the device): joining a hidden network has to write
`set_network <id> scan_ssid 1` -- before the passphrase, before the network is
selected -- for a network created on the spot and for one wpa_supplicant
already knows, and never for an ordinary visible network. A passphrase too
short to store has to be refused as a wrong passphrase without the supplicant
being told anything. The config store, sys_server and the power/clock helpers
are stubbed; `tests/stubs/` carries the two LVGL typedefs `power.h` needs so
this stays a plain-C suite. The Wi-Fi page itself -- the "Add hidden network"
row and the manual-entry page behind it -- is covered by `test_wifi_pages`
in the headless UI suite below.

## Real codec integration

Install the normal host development libraries (Debian/Ubuntu example):

```sh
sudo apt-get install build-essential python3 pkg-config libopusfile-dev libwavpack-dev
make -C sonix-player/tests codecs
make -C sonix-player/tests sanitize-codecs
```

The codec suite links the actual player decoder dispatcher and bundled `dr_mp3`/`dr_flac`, plus **real libopusfile/libopus/libogg**. Other player codecs are also compiled/linked normally, not replaced with fake decoders. `PKG_CONFIG`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `BUILD` can be overridden for a custom dependency installation.

`media_fixtures.py` creates deterministic silent MP3 frames, lossless FLAC verbatim frames with real CRCs, and Ogg Opus silence packets with valid page CRCs and preskip/end trimming. No ffmpeg/encoder command or binary media fixture is needed. Tests check exact PCM frame counts (including MP3 Xing/LAME delay/padding and Opus preskip/end trim), buffered-source identity, seek/source-span exclusions, codec mismatch rejection and canceled-source opens. Large ID3/FLAC headers and >1 MiB FLAC transfers exercise forward header skipping and ring wrap without a file cache.

A no-ID3 MP3 fixture is intentional: the bundled `dr_mp3` discards its first ten bytes when given a NULL seek callback, losing the first audio frame. The production bridge supplies **limited retained-header seeks** and refuses an unknown-length whole-stream count; this test detects both regressions.

Both sanitizer targets use AddressSanitizer and UndefinedBehaviorSanitizer. At `-O1`, GCC reports pre-existing intentionally bounded/truncating MP4 tag copies; `sanitize-codecs` leaves that warning category nonfatal, while other warnings remain fatal. The core suite keeps all warnings fatal.

## Limits

The fixture proves transport, byte/sample counts and ownership behavior on the host. Scheduling helpers are stubs and there is **no ALSA/device loop** here, so these tests do not certify real-time scheduling, gapless analog output, Wi-Fi performance, target CPU use or battery life. The TLS cases run against the OpenSSL the host has (the player's own client code, `dlopen`ed exactly as on the device, sees the host's libssl 1.1/3.x); with no loadable OpenSSL they print `SKIP TLS` and the rest of the suite still runs. Real-network trust, redirects and certificate cancellation, plus UI interaction, should also be checked on hardware. See [the V1 checklist](../../docs/subsonic.md#validation).

## Headless main-menu appearance tests

```sh
# Only needed if the player build has not already fetched LVGL:
git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl.git sonix-player/lvgl
make -C sonix-player/tests ui -j"$(nproc)"
```

This optional suite links the real LVGL layout engine, main menu, shared grid,
Appearance controls, themes, language loader and configuration store. It uses
`tests/ui/lv_conf.h` to disable SDL, FreeType and device drivers, so a compiler
and the LVGL checkout are sufficient; no audio libraries or display are needed.
Deterministic built-in font metrics and unrelated navigation/playback services
are substituted, rather than running the player or touching device hardware.

It checks the default and saved **Main menu icons only** setting on both panel
sizes, all five themes, exact icon sizes and centering, glow-free icon-only
tiles, unchanged secondary-menu tiles, restoration of the labeled layout and
its decorative glows, real switch callbacks and config save/reload,
theme/accent/tint/font changes, hidden-label translation, and the six tile
destinations. Both language asset trees are used. Icon-only images must have
native-size sources and no scaling transform, including after font-metric
reflows. Actual LVGL snapshots verify pixel-for-pixel alpha edges, theme tints
and opaque pixels in ARGB8888 and the panel's RGB565 format; geometry alone
cannot catch a small bitmap being enlarged into a blurry icon.

To inspect rendered screens, optionally set `MORPTHIS_MENU_SNAPSHOTS` to an output
prefix (its parent directory must exist), for example:

```sh
MORPTHIS_MENU_SNAPSHOTS=.build/menu make -C sonix-player/tests ui
```

This writes ignored `.build/menu-<width>-theme<kind>.ppm` files using LVGL's
software renderer. To regenerate the main-menu-only native art, see
`tools/main_menu_icons.py`; Python image tools are not required to build or
run the player/tests. The host-build workflow runs this suite as well. These
checks do not replace visual and touch testing on a physical V1.

## Headless screensaver artwork tests

The same `ui` target also builds `test_screensaver`, which links the real
screensaver, scrolling labels, themes, settings card helpers, language loader
and configuration store. The player that lends the artwork, the cover loader
and the device services are substituted. The test drives the loader by hand,
so it decides when a decode lands, what size it is, or that it found nothing.

It checks the **Album art: Fill / Framed** choice on both panel sizes, default
and saved:

- Fill is unchanged: the sleeve still covers its whole area, and it never
  asks for a decode. On the V1 that always means enlarging it.
- Framed asks for one copy of the artwork at exactly the frame's size (160 px
  on the V1, 400 px on the 480-wide panels), from the path the player read
  the artwork from. It asks as the panel goes dark, once the player has the
  artwork, and never again after a theme change or a failed decode.
- Until that copy lands, the player's sleeve stands in, never enlarged; a
  non-square Immersive crop is not used as a stand-in.
- The frame is centred in the area above the strip, with the margins, the
  player's rounded corners and no strip backdrop.
- The page behind the frame is right in all five themes.
- Small pictures are shown at their own size.
- Track changes, abandoned decodes, switching back to Fill, switching to
  Pictures, dismissing and switching off release every borrowed and owned
  buffer.

Rendered RGB565 snapshots verify that every pixel of the frame outside its
rounded corners is the decoded picture's own pixel: drawn one to one, not
shifted, not resampled. The test pictures are one-pixel checkerboards, so any
scaling fails. The three new texts are checked in every language file, and
the captioned pill row is checked to relabel on a language switch.

The same test checks the size limit the screensaver hands the Screensaver
folder. It reads the size the empty-folder note promises from every language
file, requires them all to agree, and requires the limit to match: 480x800 on
the V1, which ships the R1's texts, and the panel's own size elsewhere.

Set `MORPTHIS_SCREENSAVER_SNAPSHOTS` to an output prefix to keep the rendered
screens as ignored `.ppm` files, for example:

```sh
MORPTHIS_SCREENSAVER_SNAPSHOTS=.build/saver make -C sonix-player/tests ui
```

## Screensaver folder rules

`make -C sonix-player/tests pictures` builds and runs `test_screensaverpics`,
plain C with no LVGL. It links the real folder code (`screensaverpics.c`) and
JPEG header reader (`jpeginfo.c`), and checks them on synthetic JPEGs in a
temporary card. Only headers matter here, so each file has a JPEG's segment
structure, with filler where the picture data would be.

- **Headers:** the size and coding are read even when the frame header sits
  behind about 93 KB of an editor's metadata (Exif with a thumbnail, a colour
  profile, XMP, Photoshop resources). The metadata filler contains frame
  markers of its own, as a real Exif thumbnail does, so a reader that looked
  inside a segment would be caught. Fill bytes, truncated files, scans with no
  frame, non-JPEGs and both extensions in either case are covered too.
- **Folder rules:** pictures over the limit, landscape or progressive are
  turned away, and so are hidden files (including a Mac's `._` companions),
  other names and missing files. Pictures made to the V1's note (480x800) are
  found with the V1's limit and not with its panel size, which is the fault
  this guards against.
- **Random pick:** it only ever returns a usable picture and reaches all of
  them. It avoids the picture already up while there is another, and falls
  back to it when it is the only one. Twenty `._` files beside one picture
  never make it miss.
