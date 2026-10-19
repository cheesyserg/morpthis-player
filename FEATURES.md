# Features

---

## Supported audio formats



`.wav` `.flac` `.mp3` `.ogg` `.opus` `.m4a` `.m4b` `.mp4` `.aac` `.wv` `.wvc` `.ape` `.dsf` `.dff` `.aif` `.aiff` `.aifc` `.caf`

**CUE sheets** split a single ripped disc into up to 99 tracks, taking the format from the file the sheet points at. WAVs carrying their own RIFF markers work the same way.

**WAV and AIFF tags** are read from the ID3 chunk taggers add to them, so titles in any script and the cover come through; a WAV's own INFO list fills in whatever that leaves empty.

## Audio

- **Volume** - The same volume curve as the stock player is used.
- **Remember Volume** Four levels remembered separately, one per output: 3.5 mm, 4.4 mm, USB-C, Bluetooth.
- **Graphic EQ** - 10 bands at 31 Hz to 16 kHz, ±12 dB. Ten built-in presets and
  as many of your own as you like.
- **Parametric EQ** - 10 bands, each with frequency 20 Hz to 20 kHz, gain
  ±15 dB in tenths, Q from 0.10 to 10.00, and five shapes: peak, low shelf,
  high shelf, low-pass, high-pass. Preamp −15 to +6 dB. The page draws the real
  response, computed from the coefficients that are running. Presets are
  Equalizer APO files in the `PEQ` folder of the card: the `ParametricEQ.txt`
  that AutoEq publishes for your headphones loads as it is, and a preset saved
  on the player opens in APO.
- **MSEB** - HiBy's MageSound tuning, matched to the stock player: 10
  characteristics driven by 13 filters, with the slider travel selectable
  between 20, 40 and 100.
- **Soundfield** - mid/side width from mono to twice natural, starting at 1.50
  for an audible widening when switched on (1.00 is neutral).
- **Channel balance** - ±20 dB in half-decibel steps.
- **Crossfeed** - level, cutoff and delay, all adjustable.
- **ReplayGain** - off, track or album, read from the tags.
- **Fade** - one to twelve seconds at the track boundary.
- **Gapless** - on by default; the PCM device stays open between tracks of the same shape. A track that ends on its own hands over its queue. A manual skip (next, previous, or a track picked from a list) drops the old queue and keeps the device open, so the next track skips the device open and the DAC mute. A saved `audio.gapless = 0` turns it off.
- **DSD** - DoP by default, with nothing in the chain allowed to touch the
  samples; or converted here to 176.4 kHz PCM. DoP falls back to conversion by
  itself when the device refuses the rate.
- **DAC controls** - four digital filters, DRE, NOS, high gain, and DSD gain
  compensation from 0 to 6 dB.
- **Line out** - drives the jack at a fixed level for an amplifier, refuses
  volume changes while on, and puts your level back on the way out.
- **USB DAC** - the player becomes a USB sound card, 32 kHz to 384 kHz.
- **USB audio out** - a DAC or USB-C headphones driven with the player as host.
- **In-line remote** - the three-button cable remote: one click play/pause, two
  next, three previous.

## Library

- **Sorting across alphabets** - a collation that groups by script, folds case and accents, and skips leading articles unless you turn that off.
- **A–Z strip** - shown when scrolling a list.
- **Search** across tracks, albums and artists.
- **Albums with the same name stay apart** - an album is its name and whose it is: the album artist, the artist when there is none, and the folder when there is neither.
- **Quality badges** under a title: lossy, CD, Hi-Fi, DSD.
- **Years, Playlists and Most played** - tiles on the Music page alongside All tracks, Albums, Artists, Album artists, Genres and Browse. Every local track play is counted in the library database, and Most played lists your most-listened tracks in descending play-count order. The Music page tiles can be reordered or hidden via **Music → Settings → Library → Rearrange main page**.
- **Picture viewer** - JPEG (`.jpg`, `.jpeg`) and PNG (`.png`) images can be opened directly from the File Manager.
- **Playlists** - create, rename, reorder, delete. A long press on a row of All tracks, Albums, Artists or Album artists starts a selection, and the chosen tracks, albums or artists go into the queue, the favourites or a playlist together. Inside a playlist or the favourites the selection also takes the chosen tracks out, after asking. In the file browser the same works on tracks and folders, a folder bringing everything under it. A backup writes an extended M3U to the card that other players can read; import reads one back, resolves missing entries by file name, and reports what it found.
- **Playback modes** - normal, repeat all, repeat one, shuffle, and a continuous shuffle that deals a fresh order each round.
- **Album and folder chaining** - the next album starts when one ends, or the next folder in a depth-first walk when you came in through the browser.
- **The local queue survives a reboot**, with positions, and so does the volume. Ephemeral Subsonic queues do not.

## Now playing

- Artwork edge to edge across the top, and the same picture flipped, blurred and dimmed behind the controls.
- **Alternative layout** - the progress bar becomes a **waveform**
- **Immersive layout** - the picture itself, edge to edge across the whole panel: the sleeve fills the screen and is centre-cropped, at its own brightness, with nothing laid over it. The controls stand on a frosted band: the foot of the same photograph, lightly blurred, dimmed to the darkness the type reads on and tinted so the sleeve's colour survives in it, cut to the deck's rectangle - so the artwork is still seen through the glass, softly, while the names, the bar, the clocks and the transport are read on top; the chevron and the source marks take their chance in white over the sharp part. Anything holding artwork gets one - local files, podcasts, audiobooks, Subsonic and the streaming services alike - and without a picture the page falls back to its own colour with the controls in the theme's ink.
- **Poster layout** - the sleeve scaled to the edges of the screen without the crop Immersive takes: the whole picture from side to side at the width of the panel, held at the top with square corners and nothing stretched, and the deck below it on its own opaque ground rather than over the artwork - as much of the sleeve as the deck leaves room for, at the largest size the screen allows. Anything holding artwork gets one, and without a picture the empty sleeve stands edge to edge in the cover's colour with the controls in the theme's ink; the favourite marker is a heart on this deck, kept a step smaller than the star it stands in for; and the foot of the sleeve fades into the deck over its last pixels instead of meeting the controls at a line.
- **Player layout** - Standard, Waveform, Alternative, Immersive or Poster, in Settings → Music → Display options; the choice is remembered across restarts.
- **Tint controls from album art** - optional, off by default, in the same Display options: the play/pause button and the played part of the progress bar take the main colour of the playing track's artwork, in every layout, while previous and next keep their colour. The colour is the one already worked out from the decoded artwork, so nothing extra is decoded and playback is untouched; a track with no artwork keeps the theme's colours.
- **Cover Flow** - an album carousel, swiped up from the bottom of the Music page.
- **Queue**, **Details** and, while an audiobook is loaded, **Chapters**.

## Streaming

- **Subsonic / Gonic** - a new Streaming destination for your own server. Test the connection, browse music folders or artists/albums in pages of 200, and play the page as a queue. MP3 256/320, Opus, or original FLAC/MP3/Opus; transcoding runs on the server. Two fixed 1 MiB RAM rings isolate HTTP/TLS from decoding, refill near 50%, and pre-cache 512 KiB of the upcoming track near 90%. No growing disk cache or playback seeking; compatible-format gapless uses the existing ALSA handoff. Remembering credentials is optional and stores an unencrypted private file. See [setup and limitations](docs/subsonic.md).
- **Tidal** - sign in with the code on screen or with the QR-Code; Low, High, CD and Hi-res quality. Search, albums, artists, playlists, favourites and the featured lists. Favourites and playlists can be managed from the player.
- **Qobuz** - sign in with the account; MP3 320, FLAC 16/44.1, 24/96 and 24/192. Search, albums, artists, playlists, favourites and the featured lists. Favourites and playlists can be managed from the player.
- **Podcasts** - the Podcast Index catalogue: search, trending, follow up to 100
shows kept on the device.
- **Radio** - the radio-browser.info directory by language, country or genre,
plus search, favourites, recently played, and **My stations** read from a
`radio.txt` you write yourself in the card root. MP3 and AAC, HTTPS with real
certificate checking (plus an opt-in relaxed mode for a server of your own with
a self-signed certificate), and HLS.


## Wireless

- **Bluetooth out** - the whole stack is brought up by the player rather than by
  the firmware's scripts. Pair, connect, forget; the codec list comes from what
  the connected device announces and the best is chosen at each connection.
- **AirPods** - battery per earbud and for the case, model detection, and noise
  control: off, cancellation, transparency, adaptive.
- **Bluetooth in** - a phone or laptop connects and the player becomes its
  speaker, decoding through its own DAC. The incoming codec, rate and depth are
  shown.
- **AirPlay** - the player appears as an AirPlay target.
- **DLNA** - the player appears as a renderer.
- **Wi-Fi** - scan and join the networks in the air, and hidden ones too:
  "Add hidden network" at the foot of the Wi-Fi page takes a name and
  passphrase typed by hand (blank for an open network) and probes for the
  network by name, since it never appears in the scan list.
- **Wi-Fi transfer** - a browser on the same network gets the card: upload
  files and whole folders, rename, move, delete, download, show hidden files.
  Reachable at `morpthis-transfer.local`.
- **SonixLink** - a control protocol for a phone: the phone fetches the index
  once and queries it locally instead of paging over the wire. Playback state,
  queue, favourites, artwork and commands.
- **USB mass storage** - connect the device to a PC or a Smartphone to manage the files on the SD Card.

## Audiobooks

> Audiobooks go in the `Audiobooks` folder, in any subfolder: `Audiobooks/Author/Series/...` is read as the author and the series when the tags do not say.

- **Library, Series, Authors and Continue** - four sections, each sorted A-Z, Z-A or by date; the library also by recently listened, a series by the number of each book in it. Continue holds the books started and not finished, last listened first; the finished ones are behind the button beside the options.
- **A book is a file or a folder** - an `.m4b`, an `.mp3`, `.opus`, `.ogg` or `.flac` with chapters, or a single audio file is a book; audio files of one folder with the same album tag are one book whose files are its chapters, `CD 1` / `Disc 2` subfolders included, so several books side by side in a folder stay apart.
- **Author and series from the tags** - album artist or artist, `SERIES` / `SERIES-PART` and the movement name and number, in MP4, ID3v2 and Vorbis tags.
- The position is remembered per book, and survives a rescan.
- Chapters from the MP4 container, from ID3 frames, or from the `CHAPTERxxx` comments of Opus, Ogg Vorbis and FLAC.
- The two transport buttons are set independently to 10, 30 or 60 seconds.
- Speed 0.5x to 2.0x with the pitch preserved.
- Stop at the end of a chapter, and rewind a few seconds after a pause.
- **Sleep timer** - set a timer with the desired duration and the playback is paused automatically after it elapses.

## Settings

- **Appearance** - dark and light, and a layout **Style**: **Classic (TempoTec V1)**, the original tile layout and the default, or **Morpthis**, the compact layout with function-matched icons; twelve accent colors, clock position, a dynamic tint that borrows the accent's hue for backgrounds and cards, and a toggle to show/hide the battery percentage.
- **Appearance → Browse on home screen** - replaces the Music tile with Browse, opening the card’s file browser in the card’s `Music` folder directly; a card without that folder still opens at its root. The chevron and the root button still walk up to the rest of the card. Off by default; Music remains available when switched off.
- **Appearance → Main menu icons only** - optionally hide the six main-menu tile labels and show larger, centered icons. Dedicated native-pixel art avoids stretching small toolbar glyphs or repeatedly filtering the colored illustrations. Applies immediately in every theme and is saved across restarts; other menus, the status bar and the now-playing strip are unchanged. Off by default.
- **Appearance → Boot screen** - the picture the next power-on opens with: Retrospace (the default), Space, Travelling in space, or Stock, the firmware's own light and dark pictures. The choice is written to the flash marker the boot script reads, so it takes effect at the next boot.
- **Appearance → Font** - the face the whole interface is drawn with: MiSans (the default), Neon 80s and, where the image carries them, Roboto Mono, Roboto Condensed, Inter and Barlow Semi Condensed. Text can also be sized Normal or Large. Any `.ttf` or `.otf` file put in the `Fonts` folder on the card is offered beside those, so people can install their own; a face without Cyrillic, Hangul, kanji or Greek still draws the whole interface, because MiSans stays behind it for the letters it does not have.
- **Screen** - brightness (1% to 100%), screen-off timeout, double-tap to wake, screensaver,
  and 180° rotation with the touch rotated with it.
- **Screensaver** - choose to show the cover of the current playing track or pictures from the `Screensaver` folder. Under **Album art**, **Fill** draws the artwork behind the title, artist and clock; **Framed** shows the whole cover instead, at its own resolution, in a rounded frame with margins round it, above the words. That frame is 160 px on the V1. The cover is decoded for exactly that size and never enlarged, so it never turns into blocks.
  > Pictures must be baseline (not progressive) JPEGs, portrait, and no larger than 480x800. The V1 crops and scales them to fill its 240x320 screen. For the sharpest result with nothing cropped, make them 240x320 or 480x640, the screen's own 3:4 shape.

- **Power** - standby, LED behaviour, automatic power-off from thirty seconds to
  six hours, and a charge limit from 80 to 100 %.
- **Power → Hold power for the menu** - what a held power button does. On, the
  default, the hold brings up the power menu and switching off or rebooting
  takes a slide to confirm. Off, the same hold switches the player off where it
  stands, with nothing on the screen to read or to confirm - for a player that
  lives in a pocket, where the hand holding it wants to know the music has
  stopped without looking at it. The menu is the only place the interface offers
  a reboot, so that comes back with the option.
- **LED effects** - on the V1 and the R3 Pro II the status light is driven
  directly rather than through the firmware's preset patterns: it fades in on
  power-on, cross-fades between colours, breathes red while charging, turns
  green when the charge is done and fades out on the way down. Brightness (1-5)
  and transition speed (20 ms to 2 s) are set under Power.
- **Date and time** - set by hand and written to the RTC, 24-hour clock, and a
  time zone with its own standard and daylight choice.
- **Language** - English, Italiano, Deutsch, Español, Français, Russian, Chinese,
  Japanese. Titles and tags in Korean, Thai and Arabic are shown too, Arabic
  right to left.
- **Remap buttons** - the three media buttons and the two volume buttons, each
  assignable to nothing, play/pause, previous, next, volume up or volume down.
- **Keyboard layout** - six layouts: English, Italian, German, Spanish, French
  and Russian.
- **Screenshots** - under More: volume up and the power key save the screen to
  the card.
- **System** - device, DAC, serial number, card space, OS version, firmware
  update from a `.upt` on the card, and factory reset.
- **Developer options** - unlocked by tapping the build number: ADB, log to the
  card, database log, disabling the software volume attenuation, and a page of
  running processes and memory.
