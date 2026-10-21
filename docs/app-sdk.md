# The Morpthis app SDK

The SDK is the contract between the player and apps built outside this
repository. It is a separate repository because apps have their own release
cadence, their own authors and, for some of them, their own licences. The
player only needs the manifest and the launch path; everything an app is built
with belongs in the SDK.

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

## Repository layout

```text
morpthis-sdk/
  include/morpthis/
    app.h          // entry point, lifecycle, API version
    ui.h           // screens, widgets, events
    input.h        // buttons, touch, gestures
    audio.h        // playback control, PCM access
    fs.h           // card paths, app folder
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
- **One entry point.** `int morpthis_app_main(const morpthis_app_t *app)` or
  similar; the player calls it on the GUI thread.
- **No blocking the GUI thread.** Long work goes on an app-owned thread and
  comes back through the SDK's `post()` (the player's `gui_post`).

## API surface, first cut

Keep the first version small enough to ship:

- `app`: name, version, app folder, API version, `post()`, `quit()`.
- `ui`: screen, label, button, list, image, slider, toast.
- `input`: button events, touch events, back gesture.
- `audio`: play/pause/next/prev, current track, volume; or "leave the player's
  audio alone" for apps that do not need it.
- `fs`: the app folder, read/write helpers, card root.
- `log`: one line to the player's log.

Do not add networking, Bluetooth or DAC control in the first version. They are
the player's, and an app that needs them can ask for them later.

## Packaging

An app package is a zip with the app folder at its root:

```text
calculator-1.0.0.mapp
  app.json
  icon.png
  calculator
```

The on-card layout is the same folder under `Apps/`. A future player version
can install a `.mapp` directly; today the user extracts it.

## CI

- Build every sample with the pinned toolchain.
- Run `check-abi` against glibc 2.22.
- Package each sample as a `.mapp`.
- Upload the packages as artifacts; attach them to a release on a tag.
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

The player's app-mode reclaim (freeing the streaming pages' static arrays,
stopping playback, dropping thumbnails) is the other half of this; see
[apps.md](apps.md).

## What the SDK should not do

- It should not ship the player's source or its internal headers.
- It should not expose LVGL, FreeType or ALSA directly.
- It should not promise a stable ABI across a major player release without a
  version bump.
- It should not hide the memory budget: an app that cannot fit should fail
  early and say so.
