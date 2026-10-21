# Apps on the microSD card

Morpthis can list and start apps that live under `Apps/` at the root of the
microSD card. The player itself stays a music player: an app is a folder the
user copies onto the card, and the Apps page is the only part of the interface
that knows about it. The page is reached from **More → Apps**.

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
    calculator/
      app.json
      icon.png
      calculator
    notes/
      app.json
      icon.png
      notes
```

- `Apps/` is scanned when the Apps page opens. There is no install step and no
  registry: a folder that appears on the card appears in the list, and deleting
  it removes the app.
- The folder name is the app's identity on the card. It should match the
  manifest's `id`.
- `app.json` is required. A folder without a readable manifest is ignored, and
  the log says why.
- `entry` names the executable, relative to the app's folder. It must be a
  regular file with the execute bit set.
- `icon` is optional. When present it is drawn on the app's tile, decoded to
  the tile size; a missing or unreadable icon falls back to the generic apps
  glyph. The tiles follow the player's menu layout in Dark, Light and Tokyo
  Night.

## Manifest

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `id` | string | no | folder name | Stable identity; lowercase, no spaces |
| `name` | string | no | `id` | Shown in the list |
| `version` | string | no | empty | Semver recommended |
| `entry` | string | **yes** | — | Relative to the app folder |
| `icon` | string | no | empty | Relative to the app folder |
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
name a file outside the folder the player is about to run.

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

The player tries the in-process path first. It `dlopen`s the entry as a shared
object, checks the API version the app was built against, and calls
`morpthis_app_main()` on the GUI thread. The app creates a screen; the player
puts it up with its top bar, floating chevron and swipe-back, and destroys it
when the app quits. The chevron and swipe both deliver `MORPTHIS_BUTTON_BACK`
to an app that subscribed, then quit the app. A `game` screen is put up without
the top bar, the chevron or the gesture, and the app quits itself.

If the entry is not a shared object, or has no SDK entry point, the player falls
back to the separate-process model: fork, `execve` the entry from the card, and
wait for it. The child gets the app's folder as its working directory and the
player's environment; its signal handlers are reset to the defaults. A tool
that outlives 30 seconds is killed rather than left behind.

A file that looks like a plugin but cannot start (a missing SDK symbol, a newer
API version, an entry that returns non-zero) is reported as such; it is not
quietly retried as a tool.

## Memory

The device has 64 MB, and with the player running `MemAvailable` is around
18 MB. That is the budget a light app has to fit in. The player's own caches are
the first thing to reclaim before a large app: the streaming pages' static
arrays alone are several megabytes.

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
| No `Apps/` folder | "No apps yet..." |
| `app.json` missing or invalid | The folder is skipped; the log says why |
| `entry` missing or not executable | The row is listed; launching reports the error |
| `execve` refused | The row reports the `errno` (`EACCES`, `ENOEXEC`, ...) |
| Entry is not a shared object, or has no SDK entry point | Falls back to the tool path |
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
as root, with the player's environment, and can read and write the card. That
is consistent with the rest of the device (ADB, a root shell and firmware
updates are already available), but it means an app from an untrusted source is
as powerful as the firmware. Signing and a permission model are future work;
until then, install apps you trust.

## Current limitations

- A UI app is not sandboxed: a fault in it takes the player down.
- Heavy reclaim is best-effort; static pages of the player stay mapped.
- No install/uninstall UI: copy or delete the folder.
- No hot reload: the page rescans when it is opened.
- No permissions, signing or sandbox.
- `min_player` is read but not enforced yet.

## Roadmap

1. An app-mode reclaim of the UI caches themselves, so heavy apps get more than
   the audio path and SQLite pages.
2. `.mapp` package install from the file manager or Wi-Fi transfer.
3. Signing and a permission manifest.
