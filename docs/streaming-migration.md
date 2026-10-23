# Streaming removal and app migration

Status: **phases 1–3 complete** (home/Music layout, streaming UI, streaming back
end). The player keeps working at every phase. This checkout could not be
UI-built in the current container (LVGL/SDL build is not supported there), so
phases 2–3 were validated statically: symbol audit across `src` and `tests`,
diff review and `git diff --check`. Run the normal UI build on the build
machine before flashing.

## Goal

The player is the local music player, audiobook player, hardware front end and
app host. No commercial streaming service, podcast client or radio directory
is compiled into it.

Streaming returns as sideloaded apps under `Apps/pkgmerps/`:

- `radio.pkgmerp`
- `podcasts.pkgmerp`
- `qobuz.pkgmerp`
- `tidal.pkgmerp`

Apps use the public API 3 surface (`net.h`, `audio.h`, `fs.h`, `power.h`,
`playback.h`, `library.h`, `ui.h`) and the app data folder. The player core
must not link or include any `src/system/streaming/` or
`src/gui/streaming/` file.

AirPlay, DLNA, Wi-Fi transfer, SonixLink, Bluetooth and Last.fm are **not**
streaming services in this sense and stay in the player.

## Target home layout

Five tiles, 2 columns x 3 rows:

```
+-------------------------------+
|            Music              |  full-width
+---------------+---------------+ 
|   Wireless    |     Apps      |
+---------------+---------------+ 
|     More      |   Settings    |
+---------------+---------------+
```

- Audiobooks leaves the home screen.
- Apps takes the old Audiobooks slot.
- Streaming leaves the home screen.
- Music is the wide tile.

## Target Music layout

Audiobooks becomes a Music destination next to Browse:

- Dark/Light: a normal tile in the Music grid, immediately before Browse.
- Tokyo Night: a button in the Browse card, next to the active browse
  destinations. Search remains available from the corner menu.
- The tile arrangement settings include the new slug `audiobooks`, so users can
  move or hide it like every other Music tile.

## Inventory before removal

| Area | Files | Lines | Used by |
| --- | --- | ---: | --- |
| Radio back end | `src/system/streaming/radio.c`, `onair.c`, `streamkeys.c`, `streamturn.c` | ~4.1k | now-playing, power, quick panel, factory reset, main |
| Radio UI | `src/gui/streaming/radiopage.c` | ~1.4k | home Streaming, now-playing |
| Podcast back end | `podcast*.c`, `podcastcache.c`, `podcastdl.c`, `podcastsubs.c` | ~2.7k | now-playing, power, factory reset, main |
| Podcast UI | `podcastpage.c`, `podcastsaved.c` | ~3.3k | home Streaming, now-playing |
| Qobuz | `qobuz*.c`, UI pages | ~6.0k | now-playing badges/favourites, power, main |
| Tidal | `tidal*.c`, UI pages | ~6.5k | now-playing badges/favourites, power, main |
| Shared | `streambridge.c`, `streamkeys.c` | ~0.9k | all of the above |
| **Total** | | **~25k** | |

The core touch points that must be removed in the same change as each service:

- `src/main.c` — init, keys, cache roots, shutdown
- `src/gui/shell/gui.c` — screens, page init, screen tables
- `src/gui/shell/main_menu.c` — Streaming tile
- `src/gui/nowplaying/player.c` — source badges, favourites, podcast mode,
  cache ownership tests, stream-only transport branches
- `src/gui/shell/topbar.c`, `quickpanel.c` — service status/actions
- `src/gui/nowplaying/trackmenu.c` — service-only actions
- `src/gui/settings/lastfmsettings.c`, `musicsettings.c` — service switches
- `src/system/device/power.c` — radio/cache/network ownership tests
- `src/system/device/usb.c`, `firmware.c`, `factoryreset.c`, `system.c`
- `src/system/playback/device_state.c`, `library.c`
- `Makefile` — `streamkeys` generation
- translation resources — unused strings can stay for one release, then be
  removed from every language at once

## Port order

1. **Radio.** Smallest service, no account, a good test of streaming HTTP,
   ICY metadata, reconnect and the NETWORK power hold.
2. **Podcasts.** Feed parsing, subscriptions, downloads and resumed episodes.
   Shares the HTTP/decoder layer with Radio.
3. **Qobuz.** Account/API key, catalogue, favourites and cache. Needs the
   player's library to index downloaded files.
4. **Tidal.** Same shape as Qobuz; do it after the Qobuz app has settled the
   shared account/cache code.

## Shared app layer

Before the first port, add an app-side library under
`morpthis-apps/lib/streaming/`:

- HTTP/TLS streaming client (reuse Subsonic's vendored `http.c`/`tls.c`).
- MP3/FLAC/AAC decoders as needed, with the same raw-PCM contract as Subsonic.
- A stream player with bounded buffering, reconnect and metadata callbacks.
- Common Now Playing publishing and power holds (`AWAKE | NETWORK`).
- No access to player-private symbols; only the public SDK.

The player must not grow a second streaming stack to support the apps.

## Phases

### Phase 1 — home and Music layout

- [x] Add the plan sheet.
- [x] Move Audiobooks into Music, before Browse.
- [x] Add Audiobooks to the Tokyo Night Browse card.
- [x] Replace the home Audiobooks tile with Apps.
- [x] Remove the Streaming home tile.
- [x] Make the Music home tile full-width.
- [x] Update home icon arrays, Tokyo tones, native-icon lookup and comments.
- [x] Update the UI layout test (`tests/ui/test_main_menu.c`).

Note: the Apps tile uses `icon_files_game` until the generated native menu art
is refreshed; the test skips the old Audiobooks native art for tile 3.

### Phase 2 — remove the built-in streaming UI

- [x] Delete `src/gui/streaming/`.
- [x] Remove `streaming_screen`, `radiopage_screen`, `radiolist_screen` and
  `radiosearch_screen` from `gui.c` and the screen tables.
- [x] Remove the Streaming tile and its includes.
- [x] Remove the service-specific album routing from `trackmenu.c`.
- [x] Remove the podcast sleep-timer button and page lookup from the control
  centre.
- [x] Remove service badges, favourites and podcast transport branches from
  `player.c`.
- [x] Remove the remaining service favourite/star branches from `quickpanel.c`.
- [x] Remove service switches from settings.
- [x] Keep local playback, audiobooks, AirPlay/DLNA/BT/Last.fm working.

### Phase 3 — remove the built-in streaming back end

- [x] Delete `src/system/streaming/`.
- [x] Remove init/shutdown/cache-root/key loading from `main.c`.
- [x] Remove radio/cache ownership from `power.c`; leave app NETWORK holds.
- [x] Remove service cleanup from USB, firmware, factory reset and system.
- [x] Move `streamkeys` to `src/system/core/` and trim it to Last.fm (the name
  and file paths are kept so existing cards and images still work).
- [x] Remove HLS (`src/system/net/hls.c`), which radio alone used.
- [x] Remove the Qobuz-only `library_forget_under()` and keep skipping the
  legacy `Podcast` folder the scanner used to know about
  (`LIBRARY_LEGACY_PODCAST_FOLDER`).
- [x] Static validation: no live reference to a symbol unique to the deleted
  headers; `git diff --check` clean; no dangling includes. Host build and UI
  tests still to be run on the build machine.

### Phase 4 — Radio app

- [ ] Scaffold `morpthis-apps/apps/radio` (`app.json`, icon, Makefile).
- [ ] Shared streaming library: HTTP/TLS + MP3.
- [ ] Radio Browser directory search and favourites in app data.
- [ ] Custom station URL.
- [ ] Now Playing, background play, `AWAKE | NETWORK`, transport buttons.
- [ ] Host integration test with a local HTTP stream.
- [ ] Device test with a real station.

### Phase 5 — Podcasts app

- [ ] RSS/Atom fetch and parse.
- [ ] Subscribe/unsubscribe, episode list, downloaded list.
- [ ] Download to app data with resume/retry.
- [ ] Episode playback, skip intervals, sleep timer integration.
- [ ] Now Playing, background play, power holds.
- [ ] Host integration test with a local feed.

### Phase 6 — Qobuz app

- [ ] User-supplied app id/token screen; no keys in the firmware.
- [ ] Catalogue, album/artist/track browsing.
- [ ] Favourites and playlists.
- [ ] Stream and cache downloads; publish to the player library if wanted.
- [ ] Device test with an account.

### Phase 7 — Tidal app

- [ ] User-supplied credentials/device login.
- [ ] Reuse the Qobuz app's account/cache shell where possible.
- [ ] Catalogue, favourites, stream and cache.
- [ ] Device test with an account.

### Phase 8 — cleanup

- [ ] Remove unused translation keys and icons (`icon_qobuz_badge`,
  `icon_tidal_badge`, `icon_podcast_badge`, `player_live`, `qobuz_*`,
  `tidal_*`, `podcast_*` are still generated and compiled).
- [ ] Remove `streaming-keys.ini.example` and the seal/streamkeys tooling once
  Last.fm credentials have another home; `streamkeys` itself stays while
  Last.fm is built in.
- [ ] Update `README.md`, `FEATURES.md`, `docs/`, `apps.md`.
- [ ] Refresh prose in modules that still cite Qobuz/Tidal as their motivating
  user (`http`, `json`, `base64`, `sha1`, `md5`, `growfile`, `decode`,
  `mp4flac`, `dlna`): the code is generic and stays, the comments are
  historical.
- [ ] Check free RAM and boot time before/after.
- [ ] Full regression: local playback, audiobooks, apps, USB storage, standby.

## Acceptance tests

- Home shows five tiles; Music is full-width; no Streaming tile.
- Apps opens from the old Audiobooks slot.
- Audiobooks opens from Music, next to Browse, on Dark/Light and Tokyo Night.
- No streaming symbols in the player binary (`nm`, `check-exports`).
- No streaming screens or service names in settings.
- Local playback, audiobook resume, sleep timer, USB mass storage and standby
  still work.
- A sideloaded streaming app can play in the background, keep the device awake,
  keep Wi-Fi up, and survive the player's suspend handoff.

## Open decisions

- Whether downloaded streaming files should be indexed by the player library,
  or stay private to each app. Phase 1 assumes private app data.
- Whether the apps share one common stream player binary/library or copy the
  small codec layer. The Subsonic app is the reference implementation.
- Qobuz/Tidal credentials are user-supplied and stored in app data; do not
  reintroduce sealed keys into the firmware.
