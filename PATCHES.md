# Firmware patches

| patch | script | what it fixes |
|---|---|---|
| Touchscreen multitouch | `tools/gt9xx_multitouch_patch.py` | the panel reports five fingers, the driver lets one out |
| Touchscreen multitouch, R1 | `tools/cst8xx_multitouch_patch.py` | the panel reports two fingers, the driver lets one out |


## TempoTec V1 240×320 interface profile

The target build of this fork defaults to the TempoTec V1 profile. At runtime,
`BOARD=tempotec_v1` (also `tempotec-v1` or `tv1`) selects it explicitly, and the
host simulator can be told either way:

```bash
MORPTHIS_PANEL=240x320 ./morpthis_player_host
```

The profile is more than a font substitution. It makes `gui_config_t` use the
real 240×320 panel, a 24 px status bar and 6 px page padding; remaps the named UI
font faces to compact raster sizes; and gives shared headers, settings rows,
grids and status icons compact geometry.

The face those sizes are drawn from is not part of the profile any more. The V1
had Neon 80s as its primary face; it is now one of the choices in
Settings → Appearance → Font, beside MiSans (the default, and the face the
interface was designed with) and the other families the image carries, with the
card's own `Fonts` folder listed after them. Each family keeps MiSans at the end
of its fallback chain for the scripts it has no letters for, and the whole face
is rebuilt and the pages relaid out when the choice changes — see the comment at
the top of `src/gui/fonts/fonts.c`. Header actions use 36 px slots. The
Music landing page keeps Settings plus one overflow menu in the title row;
Playlists/Browse, Favourites and Search remain available inside that menu
without allowing four actions to reduce the heading to `Musi…`.

Now Playing on the V1 shows the blurred artwork behind the **whole screen**, as
Studio does on every player, with a horizontally centred square sleeve (about
150×150 px in the standard arrangement, its top aligned with the visible back
chevron and at least 18 px of air before the track-info row; a little taller
while the alternative arrangement keeps the deck to the waveform, clocks and
transport, soft-clipped corners throughout) floating on it; the source badges
and alternative-layout pills travel on the sleeve itself. The cover panel and the control deck are
both transparent on this board, so there is no horizontal edge under the
sleeve where the panel colour used to meet the deck's own blurred block. The
deck is not a fixed height: `compact_deck_height()` adds up the four rows --
names, bar, clocks, transport -- from the line heights of the fonts in force
(and the shorter 44 px waveform of the alternative layout), so the transport is
never lifted into the queue-position row above it. The full panel stays the
swipe and lyrics-gesture hit target.

The fourth arrangement, **Immersive**, is the sleeve at the bezel: the cover
panel, the clipped frame and the picture all take the size of the screen, the
corners square off, and the deck is emptied of its own fill so what stands
behind the controls is the artwork itself rather than a panel of the theme's
colour. The
controls keep the rows they have in the standard arrangement -- the deck's
height is the same, and the two clocks, the queue position and the transport do
not move. The picture above the deck is left at its own brightness -- a veil
over it is a veil over the thing the arrangement exists for -- and the chevron
and the source marks are read on it in white, as they are over every sleeve in
the player. A flat black scrim at half alpha used to cover the whole screen
here, and bands of gradient were tried before it -- a ramp's ends are a step in
the alpha's slope, and on this panel each of them draws a visible line across
the photograph -- but both darken a picture that the deck's frost already
carries the chrome on, so neither is part of the arrangement now. The
picture is asked for at 240×320 with COVER fit and drawn one pixel to one, so
the sleeve is not resampled on every frame.

What the controls stand on is a **frosted band**: the foot of the same
photograph, blurred lighter than every other blurred copy in the player --
`BACKDROP_BAND_BLUR_RADIUS` and `BACKDROP_BAND_BLUR_PASSES`, 4 and 2 against 9
and 3, so the photograph's colour masses stay standing in the glass instead of
washing out -- and dimmed to the darkness the type reads on
(`IMMERSIVE_FROST_PCT`, 55, against the 42 the blurs a title is read on
elsewhere get). The 80 the stock look glows at was tried first and turned a
pale sleeve into a light wash on the device, with the cream title and the
amber clocks floating on it; what the sleeve keeps of itself in the band at 55
is hue rather than light, and a tint in `make_backdrop()` -- each channel
pulled away from the pixel's own grey by `BACKDROP_BAND_SAT_PCT`, 140 -- sees
to it that the hue survives the dim: a dark ground of some colour, which is
what light type has always wanted here, and not a light ground of none.
The band is asked for
at the deck's own rectangle (240 wide by `compact_deck_height()` tall) and cut
out of the very crop the screen-sized picture was made from -- `make_backdrop()`
takes the band from the foot of that crop rather than from a crop of the band's
own aspect, because a blur whose colours come from pixels that are not on the
screen meets the photograph above it as a seam of the wrong colour. The band is
an image object of the artwork's frame, built before the words' page: over the
picture, under the source marks, under the deck, under the sheet the lyrics
slide in on. It is frosted glass and not a panel -- the artwork stays where it
is and is seen through it, softly -- and where it meets the sharp picture above
it is the one edge on the screen, the edge of the frost. The words' page
borrows Studio's look while it is up, and the band stays down under it: two
frosts on one screen would be one too many. With no artwork the band stays down
and the page is its own colour with the theme's ink, exactly as the standard
arrangement draws it.

The fifth arrangement, **Poster**, is the sleeve at the width of the screen
with the deck on its own ground below it: the picture is asked for at the
square of the panel's width and drawn one pixel to one, held at the top of the
panel with square corners and no inset, so it reaches the bezel on three sides
and nothing is cropped off its own sides and nothing is stretched -- which is
the difference from Immersive, where the photograph fills the screen and, the
screen not being square, takes its crop from the two sides. What the deck
leaves no room for is not in the picture either: the deck stands in front of
the sleeve's foot on the theme's own opaque panel colour rather than on the
blurred copy or on a frost, so the page reads as two grounds, the photograph
above and the controls below, with the one edge between them at the top of the
deck. The split of the panel is the standard arrangement's own
(`compact_deck_height()`), so the sleeve is as large as the deck allows and no
larger; the chrome on the deck keeps the theme's colours while the chevron and
the source marks take their chance in white over the sharp part, as they do
over every sleeve in the player. The favourite marker is the heart rather than
the star here, as it is in Immersive, but drawn a step under the star's size
(`POSTER_HEART_SCALE`, 22 px against 26 on the V1): the heart fills more of its
32 px canvas than the star does, and on a deck that is a row of type rather
than a picture to stand over, the smaller mark reads as a marker and not as a
second headline.

The one edge the arrangement leaves -- where the foot of the sleeve meets the
top of the deck -- is softened rather than drawn: a gradient from nothing to
the deck's own colour stands over the last `POSTER_FADE_HEIGHT` pixels of the
picture (36, the length of the ramp in the reference), so the photograph dims
into the controls and the two read as one ground. It is a style of two stops
with per-stop opacity (`poster_fade_grad`), retinted to the panel colour on
every theme change (`poster_fade_paint`), and up only while Poster has a
sleeve to melt (`poster_fade_apply`, from `cover_show()`): an empty sleeve
keeps its own foot, and the other arrangements keep the edges they have. Without artwork the empty sleeve is the
cover's own colour from edge to edge, and the blurred copy stays down: the
deck no longer shows it through, and the sleeve covers everything above it.

The **screensaver** follows the same split. Dark and Light keep the arrangement
they have always had: the artwork stops where the blurred strip begins, and the
title, the artist and the time are read on the player's own blurred copy in
light type. The two palettes added for the V1 draw it the other way round:
Amber on Black and Tokyo Night both let the artwork run the **whole** panel,
centre-cropped, and lay a translucent field of the palette's own colour over
its foot — Amber's `#000000` at half alpha, Tokyo Night's `#16161e` at 60%, a
step heavier because the violet, cyan and blue inks are dimmer than the cream
and amber ones and have less headroom over a pale sleeve. The blurred copy is
never borrowed there: a field is a colour, not a blur.

The type in that field takes the roles the palette's own Now Playing deck gives
the same things. Tokyo Night: the track name's violet `#bb9af7` for the title,
the artist line's cyan `#7dcfff` for the artist, the accent blue `#7aa2f7` for
the clock — the screensaver's own rule, which Amber set when the clock there
became its signal amber — and the palette's ordinary icon colour `#c0caf5` for
the chevron that says how to dismiss it, at the same 70% it has always worn.
Amber: cream, amber and signal orange, unchanged. A track with no artwork
behind either field sits on the palette's page colour rather than on black, so
the same screen reads the same way whether the sleeve is there or not.

The field's colour and every line of ink are written in one place, in the
screensaver's theme-refresh callback, where they were already written for
Amber. Adding Tokyo Night beside it is a second branch in three functions —
`metadata_over_art()`, `set_info_strip_backdrop()` and that callback — and
touches the Amber path nowhere.

**Framed artwork.** The album source has always drawn the player's own sleeve.
That sleeve is decoded at its own size, about 156 px a side on the V1, so
covering the screensaver's larger area means enlarging it. In Dark and Light
it is enlarged by half again, to 240 × 188. Under the palettes' field it is
doubled, to 240 × 320, and on a 240 px panel a picture enlarged that much is
made of blocks.

Settings → Screen → Screensaver now has a second row under Album and Pictures:
*Album art: Fill / Framed*, saved as `screen/screensaver_style`. Fill is the
default, and it is the arrangement described above, unchanged.

Framed decodes the track's artwork once more, from the file, in the player's
own order: embedded first, then a cover beside the track. The decode goes
straight to the frame: the whole picture fitted inside 160 × 160 with
`COVER_FIT_CONTAIN`, never enlarged. It is then drawn one pixel to one, in a
frame with the sleeve's 14 px corners. The frame sits 40 px from either edge
and 14 px above and below, over the strip's default 132 px. The 480-wide panels
get a 400 px frame. A file that only carries a small picture is shown at its
own size.

The decode is a fourth job of the cover loader. It is asked for on the redraw
that leaves the next wake's frame in the framebuffer as the panel goes dark,
so the wake is still a pointer swap. Until it lands, the player's sleeve
stands in, at most one to one. Immersive's screen-shaped crop is never used as
a stand-in, because framed it would be a slice of the record.

The copy is let go of in five cases:

- the player lets go of its picture (the release hook);
- the style changes;
- the source changes;
- the screensaver is dismissed;
- the switch goes off.

In every theme the words sit on the page rather than on a strip. Dark and
Light lay the frame on the player's full-screen blurred copy when it has one,
which is every arrangement but Immersive. The palettes use their own page
colour and their own ink.

**Pictures from the card.** The Screensaver folder only accepts pictures no
larger than the panel. That rule was written for the HiBy players, where the
panel is also the size the note promises: 480x800 on the R1, 480x720 on the R3
Pro II. The V1 ships the R1's resource tree, texts included. So its note
promised pictures up to 480x800, while its 240x320 panel let nothing that
large through, and "No pictures to show" came up over a folder of pictures
made exactly to the note.

On the V1 the limit is now the note's 480x800. The cover loader crops and
scales each picture to fill the panel, through the same scaled decode album
covers use. The screensaver test reads the size from the note in every
language and checks the limit against it.

Two smaller fixes go with it:

- The header check read only the first 8 KB of each file. An editor or phone
  that keeps its metadata (Exif with its thumbnail, a colour profile, XMP)
  puts the image size further in than that, so such pictures were turned away
  whatever their size. The check now steps from segment to segment with
  seeks, however far in the frame header sits.
- Names starting with a dot are skipped. A Mac leaves a "._" file beside every
  picture it copies to a FAT card, and each one cost a read and one of the
  random pick's eight tries.

**OLED** is the third palette added for the V1: true black, white type, white
as its accent and `#a0a0a0` for the second line, with nothing filled grey. The
page, the status bar, the Now Playing deck, the cards and the idle pills and
buttons are all `#000000`, so the layouts are carried by the type and the
glyphs alone. Now Playing goes the same way: the blurred copy of the sleeve the
V1 lays behind the standard and alternative arrangements, and behind the
lyrics, is left out, so the sleeve stands on black (`backdrop_blur_wanted()`,
asked again on a theme change, since the same picture otherwise keeps the
backdrop it has). Studio and Immersive keep theirs: there the artwork is what
the arrangement is. What would vanish with its fill gets something else
instead. A press flashes `#303030`, the only grey that is ever a fill, and only
under a finger. A quiet control (an off switch's track, a dialog's Cancel, an
idle quick-panel circle) is drawn as a 2 px `#606060` ring. A disabled switch
is faded to 40% rather than given the default theme's wash towards light grey,
which on a black track is a grey fill. A switch that is on is one solid white
pill: its ring takes the accent, and so does the track under the indicator, so
the two rounded edges have nothing dark to blend into where they meet. Tracks
stay visible, `#303030` under the progress bar and the secondary text at 40%
under a slider, since a bar with no rest reads as a stray line. Every grey the
theme draws is a multiple of eight, which RGB565 keeps exactly; a step in
between lands on different levels in the five-bit and six-bit channels and
comes out faintly green or magenta.

It takes the same compact layouts and function-matched glyphs as Tokyo Night
and Amber, with one exception: the home screen has no title or tagline, and its
grid starts directly under the status bar, the six tiles sharing the height the
header took. It is single-hue like Amber, so the places that kept Amber
monochrome — the status glyphs, the codec colours, the balanced-output gold,
the Now Playing deck, the screensaver's field — ask `theme_is_monochrome()`
now, and the deck's roles come back as steps of grey: `#888888` for the star
and the overflow, `#606060` for the idle shuffle. A white accent is also the
first one white type does not read on, so whatever is drawn on an accent fill —
a selected pill, a confirm button, the keyboard's accept key, the knob of a
switch that is on — takes `theme_on_accent()`: black above a luma of 200, white
below it. Every earlier accent, Amber's `#ffad2e` included, stays below the
line and keeps its white.

Two board matters that are not layout live beside the profile. The V1 kernel
has one switch node, `/sys/class/switch/headset`, and no `balance` node; with
nothing in either socket the headset node reads `1`, which the HiBy logic took
for a plug. `sysinfo_model_t` therefore carries `headset_switch_idle_one`, set
for the V1, and `headphone_jack_state_raw()` on such a model treats `1` as no
plug and any other value as a 3.5 mm headset (never balanced: that route has
not been exercised on this board), logging the raw value to stderr whenever it
changes so the 4.4 mm socket's encoding can be read off a real plug later.
### The white flash on wake, and what it actually was

The panel is an SLCD: its glass shows the contents of its own memory, and a
reset leaves that memory white. Writing `0` to `/sys/class/graphics/fb0/blank`
resets and re-initialises the panel and re-arms the controller's scan-out, and
the write returns before the panel's own init sequence has finished -- the
sleep-out delays alone are up to 120 ms. A frame pushed before the panel is
listening is lost without a word.

Which is why the first three attempts at this did not fix it. They were all
about the backlight: hold it at 0 (PR #12), hold it at `BRIGHTNESS_MIN`, wait
for `FBIO_WAITFORVSYNC` to report two scan-outs, raise the fallback to 300 ms
and distrust a vsync answer that comes back too fast (PR #14, #15). Every one
of them painted the interface into the framebuffer *before* the unblank. That
leaves the controller nothing to carry to the glass after it, so the panel sat
on its reset white until something unrelated -- the status-bar clock, a button
-- next happened to paint, and the backlight faded up over *that*. Holding the
backlight down cannot cover a glass that is still white when it is released,
however long the hold is.

`power_screen_on()` now does three things, in this order:

1. **The backlight goes to a true zero, not to the floor, before the unblank
   and stays there** until the picture is on the glass. `BRIGHTNESS_MIN` is a
   level the panel is *looked at through*; a white reset frame at one percent
   is still a white frame. Zero is safe here as well as darker: the blank the
   driver already performs puts the PWM at zero with the panel unpowered
   behind it, and the screen stays dark for as long as the screen is off. The
   same zero is left behind at the screen-off blank, so the level the driver
   itself restores at an unblank is already a dark one. Overridable as
   `[screen] wake_hold_level` in `device_config.ini` for a board whose driver
   needs the floor back (set it to 1).
2. **The panel's own init is waited out, still dark** (`WAKE_PANEL_SETTLE_MS`,
   120 ms), before a frame is pushed at it.
3. **A frame is pushed after the unblank, twice** (`wake_repaint()`: the kick
   from `main.c` that re-arms the video mode and the scan-out, then a full
   repaint that pans to the frame), `WAKE_PUSH_SETTLE_MS` apart, because a push
   that arrives before the panel is listening is lost in silence and there is
   no way to ask. Only then does `backlight_hold_until_panel_shows()` end the
   hold -- on two `FBIO_WAITFORVSYNC` scan-outs, believed only when they cost
   what two scan-outs cost, or on a fallback now that stands in for those
   frames rather than for the whole wake. A credible two-frame answer now has
   a 50 ms minimum total hold instead of 90 ms; the fallback remains longer.
   The configured level then fades up over the finished picture.

With a working vsync path the wake takes about a quarter of a second of dark
screen from the unblank to the fade; the conservative fallback takes a little
over a third. The backlight stays at zero until the confirmed frame is ready,
so the wait stays invisible; what was visible was the white.

Two-column, three-row pages with five entries let the fifth tile span the last
row. Compact tile captions are one fixed line with an ellipsis and no extra
line spacing, so long translations such as “Artistes d’album” cannot draw
through the card edge. Popovers are width-bounded and scroll when necessary,
and the on-screen keyboard uses a 144 px compact tray with narrower action
keys. Quick Settings retains eight controls in a 2×4 layout; its buttons,
artwork and two card heights are reduced to fit the 320 px sheet.

The **Scan** page sizes its music note to what the three lines under it leave.
The source art is 128 px, most of the gap between the page title and the
Cancel/OK button on a 320 px panel, and a centred flex column overflows at both
ends at once: the note ran up under the title while the name of the file being
read was clipped at the bottom — the column does not scroll, so nothing was
pushed off the page, it was cut. The note is now measured against the count, the
status line and that file name, and both its layout box and its draw scale are
set from the space left over: on the V1 about a quarter of the panel width, the
proportion the art has on the 480 px players, and on those the 128 px it was
always drawn at. The status line wraps inside the panel instead of running past
both edges, and the file name is one line ending in an ellipsis — which
`LV_LABEL_LONG_DOT` only draws when the label has a pinned height, so a long
name used to take a second line that was the one being cut. The fit runs again
whenever the interface font or text size changes, since that is where the line
heights come from.

The **A–Z index strip** draws as many letters as the strip is tall enough to
hold rather than all twenty-eight. On the 240×320 panel about fourteen fit; the
rest of the alphabet stays reachable because a collapsed strip maps a press by
position across the letters' span instead of by which label it landed on. Laid
out in full, the column overflowed the bar — LVGL's flex layout has no negative
gaps — so the lower letters were drawn outside it and the bottom of the strip
jumped to the wrong rows. The strip is 20 px wide on the V1 and the big letter
under the finger is an 88×84 card.

Settings pages whose layout is written in pixels rather than built from
`settingsrow` rows now carry compact counterparts for every one of those
numbers: Date and time (five rollers, the zone list, the pills and Confirm),
the first-boot language panel, the equaliser band card (which scrolls sideways
on the V1 instead of squeezing ten columns into 216 px), MSEB, the parametric
graph, brightness and keyboard type, System's microSD card, Processes, Artist
exceptions, the firmware-update card, the screensaver strip and the two
drag-to-reorder pages (Control centre, Keyboard layout). **Remap buttons** has
no V1 photo and no room for a 480-pixel one, so on that board it is the same
page as a plain list: one row per button, its action on the right, the same
chooser on a tap.

### Touch input is intentionally not guessed

This profile does **not** hard-code a `/dev/input/eventN` node or a speculative
axis transform. Event numbering and the controller's reported ABS/MT ranges
must first be captured from the actual V1 firmware. Once that report is known,
the device-specific runtime branch belongs in the launcher/input path rather
than in the screen-layout profile.


## Touchscreen multitouch

### What the hardware actually does

The R3 Pro II's panel is a **Goodix GT967**, and it reports up to five
contacts. That is not inferred from a datasheet: with the driver module
unloaded, reading register `0x814E` over I2C returns `0x81` with one finger
down and counts up to `0x85` with five.

So the contacts reach the SoC. They are lost in the driver.

### Cause 1: the frame is thrown away

`gt9xx_touch.sh` loads the module with `gtp_max_touch_number=1`, and in
`goodix_ts_work_func` that number is not a cap that clamps - it is a test that
discards:

```
lw    a0, gtp_max_touch_number
andi  s1, s1, 0xf          # contacts the controller reports in this frame
sltu  v0, a0, s1           # more than the limit?
bnez  v0, <exit>           # then drop the whole frame
```

With the limit at 1, the instant a second finger lands the driver stops
reporting **anything at all** - not "the first finger only", the entire frame
is dropped. The same number also sizes the I2C read the driver performs per
interrupt (`max_touch * 8` bytes), so even without the test it would never ask
the controller for a second contact.

### Cause 2: the rate limit inside the loop

Raising the number is still not enough, and this is the part that is a genuine
bug rather than a configuration choice.

`goodix_ts_work_func` walks the contacts of a frame in a loop, and inside that
loop there is a check against `jiffies`:

```
lw    s8, jiffies
bnez  v0, +8
sw    s8, last_jiffies     # first time only
lw    v0, last_jiffies
addiu v0, v0, 2
subu  v0, v0, s8           # last_jiffies + 2 - jiffies
bgez  v0, <next contact>   # >= 0 -> skip the report
...
sw    s8, last_jiffies     # reached only when a contact was reported
```

Read it as: report a contact only if at least three jiffies have passed since
the last contact was reported. At `HZ=100` that is 30 ms.

The loop over a frame's contacts takes microseconds. `jiffies` does not move in
that time. So the first contact of a frame is reported, `last_jiffies` is set
to the current tick, and every remaining contact of the *same frame* fails the
test and is skipped.

The check reads like a debounce that was meant to sit around the whole handler
and ended up inside the per-contact loop instead. Wherever it came from, in
this position it can only ever mean "one contact per frame".

## The fix

Copy the two files `gt9xx_touch.ko` and `gt9xx_touch.sh` out of the firmware, put them
beside the script, and run the python script:

```bash
python3 tools/gt9xx_multitouch_patch.py
```

**`gt9xx_touch.sh`** - `gtp_max_touch_number` goes from 1 to 5, so the frame
survives and the I2C read is long enough to carry five contacts.

**`gt9xx_touch.ko`** - the `bgez` that skips the report is replaced with a
`nop`, four bytes, at `.text + 0x186c`:

```
 before   1868:  005e1023   subu  v0,v0,s8
          186c:  04410046   bgez  v0,<next contact>
          1870:  8d560024   lw    s6,36(t2)

 after    1868:  005e1023   subu  v0,v0,s8
          186c:  00000000   nop
          1870:  8d560024   lw    s6,36(t2)
```

The instruction at `0x1870` is the branch's delay slot, so it already ran on
both paths; with the branch gone it simply runs as the next instruction and
execution falls into the reporting path. The arithmetic above it still runs and
its result is now discarded, which costs two instructions per contact and keeps
the patch to a single word.

The script finds that word by searching for the three-instruction sequence
rather than seeking to a fixed offset, and refuses to touch a module where the
sequence is missing or appears more than once.

### Using it

With no argument the script works on whatever sits in its own directory:

```bash
# report the state, change nothing
python3 tools/gt9xx_multitouch_patch.py --check

# apply
python3 tools/gt9xx_multitouch_patch.py

# put the stock behaviour back
python3 tools/gt9xx_multitouch_patch.py --revert
```

A directory can be named instead, to work straight on an unpacked rootfs
without copying anything:

```bash
python3 tools/gt9xx_multitouch_patch.py rootfs/module_driver
```

Either file on its own is enough to do that half of the job, and the script
says plainly that the other half is still missing.

Both files are copied to `*.orig` beside themselves before the first write, and
running the script twice does nothing the second time.

Then put the files back in the rootfs, repack and flash.

## Touchscreen multitouch, R1


### What the hardware actually does

The R1's panel is a **Hynitron CST8xx**, and it reports **two** contacts. Two
is measured, not assumed: with the immediate below set to `5` instead, a
three-finger test still only ever produced tracking ids 0 and 1.

So asking for five here would be asking for something the glass does not have.
The patch asks for two.

### Not the cause: `cst_max_touch_number`

`cst8xx_touch.sh` loads the module with `cst_max_touch_number=1`, which looks
exactly like the R3 Pro II's problem and is not it. The parameter is declared,
it has its `__param` entry, it lands in `.bss` - and **no instruction in the
module ever reads it**. Every relocation in `.text`, `.init.text`,
`.text.unlikely` and `.exit.text` was checked; the symbol is referenced by the
parameter table and by nothing else.

Changing that line alone therefore does nothing at all.

### The cause: one immediate in `hyn_ts_init`

The driver does not consult the parameter, it writes its own number into the
field the rest of the module reads, at file offset `0x171C`
(`.init.text + 0x128`):

```
 0128:  24020001   addiu v0,zero,1
 012c:  ae220058   sw    v0,0x58(s1)      # pdata->max_touch_number = 1
```

That field is written once, there, and read back in exactly two places. Between
them it decides five things:

```
hyn_ts_init        read_len = 6 * n + 3
                   buffer   = kmalloc(6 * n + 4)
                   contacts = kmalloc(28 * n)
hyn_irq_handler    if (n < (buf[2] & 0xf)) return;    drop a fuller frame
                   for (i = 0; i < n; i++)            the parse loop
```

The frame test is the R3 Pro II's behaviour again - a second finger does not
lose a contact, it loses the whole frame - but here the I2C read length, both
allocations, the test and the loop all come off the same number. So raising it
widens them together, and nothing is left inconsistent.

`6*2+3 = 15` is exactly the two-contact frame. The header is `buf[0..2]` with
the count in the low nibble of `buf[2]`, and each contact is six bytes from
`buf[3]`:

```
buf[3] bits 3..0 : x high      buf[3] bits 7..6 : event
buf[4]           : x low
buf[5] bits 3..0 : y high      buf[5] bits 7..4 : finger id
buf[6]           : y low
buf[7]           : pressure
buf[8] bits 7..4 : touch major
```

so the second contact ends at `buf[14]`, the fifteenth byte.

### The fix

One byte, the immediate:

```
 before   0128:  24020001   addiu v0,zero,1
          012c:  ae220058   sw    v0,0x58(s1)

 after    0128:  24020002   addiu v0,zero,2
          012c:  ae220058   sw    v0,0x58(s1)
```

`li v0,1` on its own occurs eight times in this module, so it is not something
to search for. The script anchors on the pair - the `addiu` together with the
`sw` that consumes it - which occurs once, and refuses to touch a module where
it is missing or appears more than once.

**`cst8xx_touch.sh`** - `cst_max_touch_number` goes from 1 to 2 as well. It
changes nothing, for the reason above; it is rewritten only so that the insmod
line and the module do not tell the next reader two different stories.
`--check` reports the number and never judges the patch by it.

### Using it

The same shape as the other one. With no argument it works on whatever sits in
its own directory:

```bash
python3 tools/cst8xx_multitouch_patch.py --check
python3 tools/cst8xx_multitouch_patch.py
python3 tools/cst8xx_multitouch_patch.py --revert
```

and a directory can be named instead, to work straight on an unpacked rootfs:

```bash
python3 tools/cst8xx_multitouch_patch.py squashfs-root/module_driver
```

Both files are copied to `*.orig` beside themselves before the first write, and
running the script twice does nothing the second time.

Note that the packer copies `module_driver/` as it finds it and does not run
this script, so the patched module is what has to live in the assets tree.

### What the player does with it

The same `gbinput.c`, unchanged: this driver speaks protocol A too. Per contact
it sends `ABS_MT_TRACKING_ID`, `ABS_MT_PRESSURE`, `ABS_MT_TOUCH_MAJOR`,
`ABS_MT_POSITION_X`, `ABS_MT_POSITION_Y`, then `SYN_MT_REPORT`; `BTN_TOUCH` and
`SYN_REPORT` close the packet.

Two differences worth writing down, neither of which needed code:

* there is no `ABS_MT_SLOT`, so the reader never switches to protocol B, and no
  `ABS_X`/`ABS_Y` either;
* `ABS_MT_TRACKING_ID` carries the real finger id and is **never** `-1`. A
  finger lifting is said by a frame with fewer contacts and by `BTN_TOUCH 0`,
  both of which the reader already treats as the last word.

`GBINPUT_MAX_CONTACTS` stays 5 on both players. It is an array size, not a
request, and a driver that reports two fills two slots.
