# The Morpthis apps repository

This is the repository for the apps themselves, built against the SDK. It is
separate from the player and the SDK so an app can be released, versioned and
licensed on its own.

## Layout

```text
morpthis-apps/
  apps/
    calculator/
      app.json
      icon.png
      src/main.c
      Makefile
    notes/
      ...
  sdk/                 // git submodule or a released SDK package
  tools/
    package.sh
    validate-manifest.py
  .github/workflows/build.yml
```

- One directory per app in this repository. The directory name is the app id
  and the name of the package on the card.
- Each app owns its `app.json`, icon, source and build. There is no shared
  `main.c` and no app registry to edit.
- The top-level build loops over `apps/*`; adding an app is adding a directory.

## Manifest conventions

- `id` equals the directory name.
- `version` is semver.
- `entry` is the binary name the build produces.
- `icon` is `icon.png` (square, 64×64 or 128×128).
- `description` is one sentence, no trailing period.
- `min_player` is the oldest player build the app was tested on.
- `category` is `app` (the default) or `game`. A game gets the whole panel: no
  title row, no chevron, no swipe-back.
- `tier` is `light` unless the app cannot fit otherwise; `heavy` stops playback
  and releases the player's caches before loading. Use it for an emulator or a
  game, not for a utility.

Validate every manifest in CI. A manifest that names a path outside its folder
is a build failure, not a runtime surprise.

## Build

- Each app has a small `Makefile` that includes the SDK's `app.mk`.
- The SDK supplies the compiler, flags, ABI check and packaging.
- C apps link statically or against the device's libc; C++ apps link
  `-static-libstdc++ -static-libgcc`.
- The build output is a `.pkgmerp`: an uncompressed squashfs image that the
  player mounts, not a folder it has to extract.

## Packaging

```sh
make package
```

writes `dist/<id>.pkgmerp`. The image holds the app folder at its root:

```text
calculator.pkgmerp
  app.json
  icon.png
  calculator
```

The name carries no version: one package per app, replaced in place on the
card. A user copies it to `Apps/pkgmerps/<id>.pkgmerp`; the app's writable
files live in `Apps/merp_data/<id>/`, which survives the replacement. There is
no install step in the player yet.

## CI

- Every push validates the manifests and runs the host tests in
  `morpthis-apps`.
- Device builds live in `morpthis-build`: it builds the pinned toolchain, runs
  `check-abi`, packages the `.pkgmerp` files and uploads one artifact per app
  plus the whole set as a zip.
- On a tag, the packages are attached to a release. A tag names the app and the
  version: `calculator-v1.0.0`.

## Releases

- One GitHub release per app version, with the `.pkgmerp` attached.
- The release notes say what changed and the minimum player build.
- The player's Apps page does not download anything; the user copies the
  package to the card.

## Testing

- Host simulator for UI logic.
- QEMU for the MIPS binary.
- On-device: launch, input, audio, back, card removal, low memory.
- A smoke test that the app starts, draws one frame and exits cleanly.

## Contribution rules

- No blocking the GUI thread.
- No allocation in the draw path.
- Log one line on start and one on exit.
- Keep the app folder self-contained; no absolute paths.
- Do not ship ROMs, copyrighted assets or credentials.
- Every app has a `LICENSE`.

## First apps

Start with the ones that prove the API without needing the display handoff:

1. **Calculator** — pure UI, no audio, no files. (light)
2. **Notes** — text input and the card. (light)
3. **Stopwatch / timer** — timers and background work. (light)
4. **Unit converter** — lists and a keyboard. (light)
5. **File checksum** — a headless tool, proves the separate-process path.
6. **Gearboy** — the first large app; it needs audio, input and a frame loop,
   and it is the reference for the in-process model. (heavy)

## What not to put in this repository

- ROMs, BIOS files or copyrighted game assets.
- Streaming credentials or API keys.
- A copy of the player's source. The SDK is the interface; the player is a
  separate repository.
- Build outputs. Releases carry the `.pkgmerp`; the repository carries the
  source.
