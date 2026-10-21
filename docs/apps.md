# Apps on the microSD card

Morpthis can list and start apps that live under `Apps/` at the root of the
microSD card. The player itself stays a music player: an app is a folder the
user copies onto the card, and the Apps page is the only part of the interface
that knows about it. The page is reached from **More → Apps**.

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
- `icon` is optional. The current page draws a generic glyph in Tokyo Night and
  a plain row in Dark/Light; per-app icons are a follow-up (see
  [Current limitations](#current-limitations)).

## Manifest

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `id` | string | no | folder name | Stable identity; lowercase, no spaces |
| `name` | string | no | `id` | Shown in the list |
| `version` | string | no | empty | Semver recommended |
| `entry` | string | **yes** | — | Relative to the app folder |
| `icon` | string | no | empty | Relative to the app folder |
| `description` | string | no | empty | One line under the name |
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
  "min_player": 1
}
```

`entry` and `icon` are refused if they are absolute or contain `..` or a
backslash. A manifest is data from a removable card; it must not be able to
name a file outside the folder the player is about to run.

## Launch model

Today the player forks, `execve`s the entry binary from the card, and waits for
it. The child gets the app's folder as its working directory and the player's
environment; its signal handlers are reset to the defaults. The Apps page
reports the exit status as a toast and logs the same lines under `apps:`.

This is enough for tools that run and exit. It is **not** enough for a UI app:
the player owns the framebuffer, the touch panel, the ALSA device and the DAC,
and a second process cannot share them. The SDK's in-process model is the
answer for UI apps (see [app-sdk.md](app-sdk.md)); until it exists, the Apps
page kills an app that outlives 30 seconds rather than leaving a worker thread
and a child behind.

## Memory

The device has 64 MB, and with the player running `MemAvailable` is around
18 MB. That is the budget an app has to fit in. The player's own caches are
the first thing to reclaim before a large app: the streaming pages' static
arrays alone are several megabytes. See the memory notes in
[app-sdk.md](app-sdk.md#memory).

## Failure handling

| What happened | What the page shows |
| --- | --- |
| No card | "No card" |
| No `Apps/` folder | "No apps yet..." |
| `app.json` missing or invalid | The folder is skipped; the log says why |
| `entry` missing or not executable | The row is listed; launching reports the error |
| `execve` refused | The row reports the `errno` (`EACCES`, `ENOEXEC`, ...) |
| App exits | `name: exit N` |
| App killed by a signal | `name: signal N` |
| App outlives 30 s | `name: timed out` |

## Security

There is no sandbox, no signature check and no permission model. An app runs
as root, with the player's environment, and can read and write the card. That
is consistent with the rest of the device (ADB, a root shell and firmware
updates are already available), but it means an app from an untrusted source is
as powerful as the firmware. Signing and a permission model are future work;
until then, install apps you trust.

## Current limitations

- Per-app icons are not drawn yet; the list uses a generic glyph.
- The launch model is a separate process, so UI apps cannot take over the
  screen, touch or audio yet.
- No install/uninstall UI: copy or delete the folder.
- No hot reload: the page rescans when it is opened.
- No permissions, signing or sandbox.
- `min_player` is read but not enforced yet.

## Roadmap

1. Per-app icons decoded from the card.
2. The SDK's in-process app API (display, touch, audio, timers).
3. An app-mode memory reclaim before a large app starts.
4. `.mapp` package install from the file manager or Wi-Fi transfer.
5. Signing and a permission manifest.
