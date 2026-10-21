# The Morpthis app SDK

The SDK is the contract between the player and apps built outside this
repository. It is a separate repository because apps have their own release
cadence, their own authors and, for some of them, their own licences. The
player owns the launch path and the implementation of the API; everything an
app is built with belongs in the SDK.

## What the SDK has to solve

The device is a 64 MB, single-core MIPS32r2 player. The player owns the
framebuffer, the touch panel, the ALSA device and the DAC. An app that wants to
draw, take input or play sound has to go through the player, not around it.
That is the central design constraint, and it decides the API shape.

Two models are possible:

| Model | How it works | Isolation | Cost |
| --- | --- | --- | --- |
| In-process plugin | The player `dlopen`s a shared object and calls its entry point | None: a crash takes the player down | Shares LVGL, fonts, input and audio; smallest memory |
| Separate process | The player hands the display/input/audio to a child | Full | Needs a handoff protocol and a second UI stack; does not fit the 18 MB headroom |

**Recommendation: in-process plugin for UI apps.** It is the model the removed
Gearboy integration used, and the only one that fits the memory budget. Keep
the separate-process path for headless tools.

The player now implements this: `dlopen` the entry, check
`morpthis_app_api_required()`, call `morpthis_app_main()` on the GUI thread, and
fall back to `execve` only when the file is not a plugin. The handoff details,
the exported-symbol list and the teardown rules are in the SDK's
`docs/player-integration.md`.

## Repository layout

```text
morpthis-sdk/
  include/morpthis/
    app.h          // entry point, lifecycle, API version
    ui.h           // screens, widgets, events
    input.h        // buttons, touch, gestures
    audio.h        // playback control, PCM access
    fs.h           // card paths, package and data folders
    log.h          // logging
    version.h      // MORPTHIS_APP_API_VERSION
  src/             // SDK runtime, if the API needs a shim
  cmake/
    morpthis-toolchain.cmake
  toolchain/
    Dockerfile     // the exact cross toolchain
    fetch.sh       // or a prebuilt tarball
  samples/
    hello/
    calculator/
    notes/
  docs/
  .github/workflows/build.yml
```

## Toolchain

Pin the same toolchain the player uses:

- GCC 9.5.0, binutils 2.38, glibc 2.27 sysroot (`rockboxdev.sh`).
- MIPS o32, `-march=mips32r2`, soft-float, `-mno-madd4`.
- The device's glibc is 2.22. A plugin may not ask for a newer versioned
  symbol; the SDK's CI must run the same `check-abi` the player's Makefile
  runs, and fail the build when a symbol is too new.
- C++ apps must link `-static-libstdc++ -static-libgcc`: the device has
  GLIBCXX_3.4.21 (the GCC 5 ABI) and the toolchain emits up to 3.4.26.

Ship the toolchain as a Docker image (or a prebuilt tarball) so an app author
does not have to build binutils, GCC and glibc to compile a calculator.

## ABI

- **C API, versioned.** `MORPTHIS_APP_API_VERSION` in `version.h`; the player
  refuses a plugin built for a newer major version.
- **The player exports the API.** It must be linked with `-rdynamic` (or an
  explicit version script) so a plugin can resolve the API symbols at load
  time. Only the API is exported; the player's internals stay hidden.
- **Opaque handles.** No structs whose layout the player can change.
- **No LVGL in the public headers.** Wrap the widgets the apps need. Exposing
  `lv_obj_t` pins every app to LVGL 9.6 and to `lv_conf.h`; a wrapper can
  change the implementation without breaking apps.
- **No exceptions across the boundary.** C++ apps catch their own.
- **One entry point.** `int morpthis_app_main(const morpthis_app_t *app)`; the
  player calls it on the GUI thread.
- **No blocking the GUI thread.** Long work goes on an app-owned thread and
  comes back through the SDK's `post()` (the player's `gui_post`).
- **A tier in the manifest.** A `light` app leaves playback and caches alone; a
  `heavy` app asks the player to stop playback and release its caches before
  loading. See [apps.md](apps.md#app-tiers).

## API surface, first cut

Keep the first version small enough to ship:

- `app`: name, version, package folder, data folder, API version, `post()`,
  `quit()`.
- `ui`: screen, label, button, list, image, slider, toast; rotation, opacity and
  `raise` for image overlays and moving parts; a canvas the app draws into
  (`create`, `buffer`, `invalidate`) and `refresh()` to put it on the panel now.
- `input`: button events, touch events, back gesture.
- `audio`: play/pause/next/prev, current track, volume; or "leave the player's
  audio alone" for apps that do not need it. Plus a raw PCM stream
  (`open`/`write`/`close`) for an app that mixes its own effects.
- `fs`: the package root, the writable data folder, read/write helpers, card
  root, and a resolve call for libraries that open files themselves.
- `log`: one line to the player's log.

Do not add networking or Bluetooth in the first version. They are the player's,
and an app that needs them can ask for them later. The DAC is different: an app
can take it, but there is no mixer, so it is one writer at a time and the app
closes the stream before it quits.

## Packaging

An app package is one uncompressed squashfs image with the app folder at its
root:

```text
calculator.pkgmerp
  app.json
  icon.png
  calculator
```

The player mounts it read-only through a loop device, so there is no install
step and no extracted copy: the file the user copies is the file that runs.
The package name carries no version, because the card holds at most one package
per app id; replacing the file is the update.

An app's writable state lives beside it in `merp_data/<id>/`, never inside the
image. `fs.h` presents the two as one tree: reads fall back from data to
package, writes go to data, and a shipped file opened for update is copied to
data first. That is what lets a package be replaced without losing a save.

While the Apps page, a loaded app or a tool is using the card, the player keeps
USB mass storage away from it: exporting the raw device would unmount the card
under the running squashfs mount. If a PC attaches, the player asks before
taking the card; confirming closes the app through its normal teardown and
exports the card. An app with threads, a network connection or the raw audio
output should export `morpthis_app_shutdown()` so that close stops its work
cleanly; the player calls it once on the GUI thread before destroying screens
and unloading the shared object.

## CI

- Build every sample with the pinned toolchain.
- Run `check-abi` against glibc 2.22.
- Package each sample as a `.pkgmerp`.
- Upload one artifact per package and the whole set as a zip; attach them to a
  release on a tag.
- Optionally run a QEMU smoke test of a headless sample.

## Versioning

- SDK semver; `MORPTHIS_APP_API_VERSION` is the major.
- `min_player` in the manifest is the minimum player build.
- A plugin built for API 1 runs on every player that advertises API 1.

## Testing

- A host build of the SDK (SDL2) for UI logic, like the player's simulator.
- QEMU for the MIPS binary and the ABI check.
- An on-device checklist: launch, input, audio, back, card removal, low memory.

## Memory

The app budget is what `MemAvailable` says, around 18 MB with the player idle.
The SDK should:

- Document a per-app budget (start at 4 MB for a utility, 8 MB for a game).
- Provide a `morpthis_mem_available()` call so an app can refuse to start.
- Encourage loading assets on demand, not all at once.
- Never allocate in the draw path.

Packages are uncompressed squashfs images. The kernel serves their data straight
from the card through the page cache, so an asset costs no decompression
workspace and no extracted copy; a large package is a card-space problem, not a
RAM one.
- Declare `tier` honestly. `heavy` is the escape hatch for a game or emulator:
  the player stops playback, releases the audio path and SQLite page caches,
  and drops the decoded artwork and list thumbnails first. It does not make
  static player pages disappear, so it is not a licence to use whatever is
  left.

The player's reclaim is `apps_reclaim_for_heavy()` in
`src/system/apps/apps_reclaim.c`; see [apps.md](apps.md#app-tiers).

## What the SDK should not do

- It should not ship the player's source or its internal headers.
- It should not expose LVGL, FreeType or ALSA directly.
- It should not promise a stable ABI across a major player release without a
  version bump.
- It should not hide the memory budget: an app that cannot fit should fail
  early and say so.
