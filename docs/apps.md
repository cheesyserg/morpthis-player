# Apps on the microSD card

Morpthis can list and start apps that live under `Apps/pkgmerps/` at the root
of the microSD card. The player itself stays a music player: an app is one
package file the user copies onto the card, and the Apps page is the only part
of the interface that knows about it. The page is reached from **More → Apps**.

A UI app runs inside the player through the SDK, on its own page with the
player's top bar, chevron and swipe-back. A headless tool still runs as a child
process. Each app declares a **tier**: a light app keeps the player's playback
and caches, a heavy app asks the player to release them first. It also declares
a **category**: an `app` keeps the player's chrome, a `game` gets the whole
panel and none of it.

This document is the on-card contract. The SDK and the apps repository are
described in [app-sdk.md](app-sdk.md) and [apps-repo.md](apps-repo.md).

## On-card layout

```text
<card>/
  Apps/
    pkgmerps/
      calculator.pkgmerp
      notes.pkgmerp
  merp_data/
    calculator/        calculator's own files
    notes/             notes' own files
```

- `Apps/pkgmerps/` is scanned when the Apps page opens. There is no install
  step and no registry: a package that appears on the card appears in the list,
  and deleting it removes the app.
- A `.pkgmerp` is an uncompressed squashfs image with `app.json`, the optional
  icon and the entry binary at its root. It is mounted read-only through a loop
  device for the moment the page reads it, and again for the app's lifetime.
  Nothing is extracted to the card.
- The package name, without the extension, is the app's identity on the card
  and its data folder name. It should match the manifest's `id`.
- `app.json` is required inside the image. A package without a readable
  manifest is ignored, and the log says why.
- `entry` names the executable, relative to the package root. It must be a
  regular file with the execute bit set.
- `icon` is optional. When present it is copied out of the mounted package and
  decoded to the tile size; a missing or unreadable icon falls back to the
  generic apps glyph. The tiles follow the player's menu layout in Dark, Light
  and Tokyo Night.
- `merp_data/<id>/` is the app's writable folder. The SDK reads from it first
  and falls back to the package, and sends every write there, so a package
  update never overwrites a save or a setting. The folder is created when the
  app is launched.

## Manifest

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `id` | string | no | package name | Stable identity; lowercase, no spaces |
| `name` | string | no | `id` | Shown in the list |
| `version` | string | no | empty | Semver recommended |
| `entry` | string | **yes** | — | Relative to the package root |
| `icon` | string | no | empty | Relative to the package root |
| `description` | string | no | empty | One line under the name |
| `category` | string | no | `app` | `app` or `game`; a game owns the panel |
| `tier` | string | no | `light` | `light` or `heavy`; heavy reclaims player memory first |
| `min_player` | number | no | 0 | Minimum player build; 0 = no requirement |

Example:

```json
{
  "id": "calculator",
  "name": "Calculator",
  "version": "1.0.0",
  "entry": "calculator",
  "icon": "icon.png",
  "description": "A four-function calculator.",
  "tier": "light",
  "min_player": 1
}
```

`entry` and `icon` are refused if they are absolute or contain `..` or a
backslash. A manifest is data from a removable card; it must not be able to
name a file outside the image the player is about to run.

## App tiers

| Tier | What the player does | Use it for |
| --- | --- | --- |
| `light` (default) | Nothing is unloaded. Playback carries on, the library and thumbnail caches stay warm, and the app fits in the free memory. | Everything that can. Calculator, notes, a timer. |
| `heavy` | Stops playback, waits for the decoder and PCM to close, releases the SQLite page caches and trims the heap before loading. The queue is kept. | The rare app that needs the room: a game, an emulator, a large image viewer. |

Heavy is a request, not a promise: static pages of the player cannot be freed,
and the reclaim only covers what the player knows how to release. The player
logs `MemAvailable` before and after, so the effect is visible. A heavy app that
needs the player's audio should be light instead, because heavy stops the
transport (the app can start it again through the SDK).

## App categories

| Category | What the player does | Use it for |
| --- | --- | --- |
| `app` (default) | The title row, the floating chevron and the swipe-back gesture stay. `morpthis_ui_height()` is the content area below the title. | Anything built from widgets. |
| `game` | No title row, no chevron and no edge swipe; the app's 0,0 is the first pixel of the panel and `morpthis_ui_height()` is the full display. | A game that draws its own canvas. |

A game is left through a control of its own; the player does not provide a back
affordance. The one piece of chrome left is the top edge: a downward swipe
brings the status bar back for a second, and a second swipe while it is up
opens the control centre. The bar hides itself again after the peek, and when
the control centre closes.

Category and tier are independent. A game is usually heavy because it wants the
room, but a small game can be light, and a heavy app that is not a game keeps
the player's chrome.

## Launch model

The player mounts the package read-only through a loop device, then tries the
in-process path. It `dlopen`s the entry from the mount point, checks the API
version the app was built against, and calls `morpthis_app_main()` on the GUI
thread. The app creates a screen; the player puts it up with its top bar,
floating chevron and swipe-back, and destroys it when the app quits. The
chevron and swipe both deliver `MORPTHIS_BUTTON_BACK` to an app that subscribed,
then quit the app — unless the app answered by calling
`morpthis_app_background()` (it keeps playing or working in the background) or
`morpthis_app_back_handled()` (it moved to another of its own screens). A
backgrounded app keeps its screens, threads and raw audio; a `game` screen is
put up without the top bar, the chevron or the gesture, and the app quits
itself. The mount is released after the app is unloaded, or immediately when
the launch fails.

An app that holds the raw output can publish its track with
`morpthis_audio_set_track()`. The status bar's play glyph and the control
centre's now-playing card then show that track instead of the player's stopped
queue; the card's play/pause, previous and next send the app the same buttons,
and tapping the card brings the app forward with `NOW_PLAYING`, which its button
handler turns into its own now-playing page.

One app runs at a time. Tapping a tile whose app is loaded in the background
brings its screen back; tapping a different tile puts up a confirmation that
closes the running app first. The outgoing app's `morpthis_app_shutdown()` runs
on a worker with a three-second bound. If it returns, its screens and shared
object are freed and its package unmounted as usual; if it does not, the app is
force-closed: the player stops waiting, releases the DAC, and leaves the shared
object, its screens and the package mount in place rather than unmapping code a
stuck thread may still be inside. The log says `did not stop ... force-closing
it`, and the zombie entry stays for the rest of the boot. The new app starts
only after the old one has let go of the card and the DAC.

If the entry is not a shared object, or has no SDK entry point, the player falls
back to the separate-process model: fork, `execve` the entry from the mount
point, and wait for it. The child gets the package root as its working
directory, `MORPTHIS_APP_FOLDER` and `MORPTHIS_APP_DATA` in its environment, and
the player's environment otherwise; its signal handlers are reset to the
defaults. A tool that outlives 30 seconds is killed rather than left behind.

A file that looks like a plugin but cannot start (a missing SDK symbol, a newer
API version, an entry that returns non-zero) is reported as such; it is not
quietly retried as a tool. A package that cannot be mounted is reported the same
way.

## USB mass storage

USB mass storage is held off while the Apps feature is in use: while the Apps
page is on screen, a UI app is loaded, or a tool child is running. The app's
package is a squashfs image on the card, and an export unmounts the card and
hands the raw device to the PC, which would pull it out from under the running
app.

If a PC does attach while the Apps feature holds the card, the player asks
whether to use USB storage mode. Confirming closes the running app through its
normal teardown (the optional `morpthis_app_shutdown()` hook runs first, so it
can stop its threads and audio), leaves the Apps page and exports the card.
The USB controller holds one gadget at a time, so confirming also switches ADB
off; turning ADB back on in Developer options takes the controller back.
Cancelling leaves the card with the player and ADB as it was; the PC sees a
card reader with no medium. Opening the Apps page later takes the card back
again, the same way DAC mode does.

## Memory

The device has 64 MB, and with the player running `MemAvailable` is around
18 MB. That is the budget a light app has to fit in. The player's own caches are
the first thing to reclaim before a large app: the streaming pages' static
arrays alone are several megabytes.

A package is uncompressed squashfs: data, inode, fragment and xattr blocks are
all stored as-is. The superblock still names a compressor, and the device
kernel must have it even when nothing is compressed; packages name LZO because
the stock rootfs uses LZO and the kernel has that decompressor, not zlib.
`-b 4096` keeps the driver's one-block input buffer at 4 KB. The kernel maps the
blocks straight from the card through the page cache: no extraction pass and no
second copy of the app's assets in RAM. Mounting costs a loop device and a
directory, not a copy of the image.

A **light** app leaves everything alone. A **heavy** app triggers the reclaim
before it loads:

- `audio_release_all()` stops playback and waits for the decoder, its open
  `FILE*` and the PCM to be gone; the queue survives, so play starts the current
  track again.
- `library_release_memory()`, `audiobookdb_release_memory()` and
  `cover_cache_release_memory()` give SQLite's page caches back without closing
  the databases.
- `player_release_artwork()` and `coverloader_release_all()` drop the decoded
  cover, its blurred backdrop and the list thumbnails; the player asks for the
  current track's picture again when the app exits.
- `malloc_trim(0)` returns the freed arenas to the kernel.

The app is still in-process, so a fault in it takes the player down. See the
memory notes in [app-sdk.md](app-sdk.md#memory).

## Failure handling

| What happened | What the page shows |
| --- | --- |
| No card | "No card" |
| No `Apps/pkgmerps/` folder | "No apps yet..." |
| `app.json` missing or invalid | The package is skipped; the log says why |
| Package is not a squashfs image | The package is skipped; the log says why |
| Package uses a compressor the kernel lacks | `mount: Invalid argument`; the kernel log names it (`Filesystem uses "zlib" compression. This is not supported`). Rebuild with the kernel's compressor, LZO on the V1. |
| `entry` missing or not executable | The row is listed; launching reports the error |
| `execve` refused | The row reports the `errno` (`EACCES`, `ENOEXEC`, ...) |
| Entry is not a shared object, or has no SDK entry point | Falls back to the tool path |
| Package cannot be mounted | `name: mount: <errno>` |
| Plugin cannot be loaded | `name: <loader error>`, e.g. `undefined symbol: ...` |
| App needs a newer API | `name: needs a newer player` |
| `morpthis_app_main` returns non-zero | `name: entry returned N` |
| App returns without a screen | `name: did not load a screen` |
| App calls quit | Back to the Apps page |
| Tool exits | `name: exit N` |
| Tool killed by a signal | `name: signal N` |
| Tool outlives 30 s | `name: timed out` |

## Security

There is no sandbox, no signature check and no permission model. An app runs
as root, with the player's environment, and can read and write the card. A
package is mounted from the card through a loop device, so the kernel's
squashfs parser is part of the attack surface; a malformed image is refused by
the mount. That is consistent with the rest of the device (ADB, a root shell and
firmware updates are already available), but it means an app from an untrusted
source is as powerful as the firmware. Signing and a permission model are future
work; until then, install apps you trust.

## Current limitations

- A UI app is not sandboxed: a fault in it takes the player down.
- Heavy reclaim is best-effort; static pages of the player stay mapped.
- No install/uninstall UI: copy or delete the package, and delete
  `merp_data/<id>/` to remove its data too.
- No hot reload: the page rescans when it is opened.
- No permissions, signing or sandbox.
- `min_player` is read but not enforced yet.

## Roadmap

1. An app-mode reclaim of the UI caches themselves, so heavy apps get more than
   the audio path and SQLite pages.
2. Installing a `.pkgmerp` from the file manager or Wi-Fi transfer.
3. Signing and a permission manifest.
