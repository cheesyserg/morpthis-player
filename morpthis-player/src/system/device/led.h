#ifndef LED_H
#define LED_H

#include <stdbool.h>

// The RGB status LED.
//
// On the TempoTec V1 and the R3 Pro II the light is an SGM31324 RGB controller
// on I2C bus 0. The firmware's leds_sgm31324_add.ko owns it and registers a
// table of preset colour patterns, exposed as small integers on
// /sys/class/leds/sgm31324-leds/led_pattern; the stock binary just writes the
// pattern number for the state it is in. The numbers below come out of its
// logical->pattern table (FUN_0047ce00) and its callers:
//
//   1   aqua           device on, nothing playing (written at stock startup)
//   2   yellow         PCM at or below 48 kHz
//   3   aqua           PCM above 48 up to 192 kHz
//   4   orange         PCM above 192 kHz
//   5   off            the logical-0 state (all-zero registers)
//   6   red breathing  charging: the stock LED state machine forces this,
//                      last, whenever its charger flag is set; the register
//                      block (blink period, ~50% duty, ramp, current on the
//                      red channel only) is the charging pulse
//   11  white          DSD (the white LED on GPIO PF11, wled_pattern_id=11)
//   8   green          MQA
//   9   blue           MQA Studio
//   10  magenta        MQA rendering
//
// This player does better than writing a pattern number: led_init() drives the
// controller directly over I2C (led_i2c.h), which is what lets it fade in on
// power-on, cross-fade between colours, breathe the charging red in hardware
// and fade out on the way down. The firmware driver is disabled by the packer
// (morpthis_firmware_packer.sh step 6b) so the controller is free from boot; where
// it is still loaded, led_i2c_init() unloads it. The pattern numbers above
// remain the fallback, used only when the controller cannot be taken over or on
// the R1, which has no SGM31324 at all.
//
// The states, and their priority when they overlap (matching the stock state
// machine, with the charge indicator on top):
//
//   charging red > charged green > playback colour > idle aqua
//
// The playback colour is the quality tier, not the bare sample rate, so an MP3
// and a FLAC of the same rate differ:
//
//   lossy (mp3, aac, ogg, opus)  yellow
//   CD (lossless, 16-bit <=48k)  aqua
//   Hi-Fi (lossless above that)  orange
//   DSD                          white
//
// The R1 has no RGB controller: leds_pwm_add.ko registers two plain LEDs,
// /sys/class/leds/red and /sys/class/leds/blue. There every lit state becomes
// blue at brightness 50 and off stays off, as its stock player does, and the
// charging red is the red LED with its "breathing" trigger. The same states,
// priorities and switches apply; only the output differs. Which player it is
// comes from the nodes present, the pattern node winning.
//
// Everything degrades to a no-op when the sysfs nodes are missing (the host
// build, or firmware without the module loaded).

// Probes the sysfs nodes, takes the SGM31324 over from the driver where it can,
// and replays the stock startup sequence: red trigger released, then a fade-in
// to the power-on aqua.
void led_init(void);

// Red breathing while a charge is actually going in, overriding the playback
// colour like the stock player does. Not "a cable is in": a battery at 100%%,
// or held at the configured charge limit, has nothing left to indicate and the
// LED goes back to whatever it would be showing with no cable -- the playback
// colour, a mode's colour, or the idle aqua. The red is an override on top of
// the rest, not a state of its own.
//
// Cheap to call repeatedly: it only writes when the resolved state changes.
// But the caller must hand it a settled answer -- the breathing is a ramp
// programmed into the LED controller, and rewriting it restarts the ramp, so a
// reading that flaps comes out as a red that stutters.
void led_set_charging(bool charging);

// Whether a cable is supplying at all, charge finished or not. While a charge is
// going in it is the red above; once the charge is done it is the green that
// says so. Either way the cable keeps the darken-in-standby option off the LED:
// without it a player left plugged in overnight answers the end of its charge
// by going dark.
void led_set_on_charger(bool present);

// Picks the state for what is playing (shown when not charging). `playing`
// false or rate 0 falls back to the idle aqua. The colour is the quality tier
// -- lossy, CD, Hi-Fi or DSD -- so an MP3 and a FLAC of the same rate differ:
// `lossy` is audio_stream_is_lossy(), `bits` the source depth (0 while unknown)
// and `dsd` true for a DSD source. A DSD file goes to the DAC over DoP, at a
// PCM carrier rate of 176.4 kHz and up that would otherwise read as hi-res PCM.
// `podcast` true paints the purple instead. Cheap to call repeatedly: only
// writes on change.
void led_update_playback(bool playing, int sample_rate, bool lossy, int bits, bool podcast, bool dsd);

// Wi-Fi transfer active: a blinking blue. Stays lit with the screen off, since
// it indicates the server is running, and overrides the darken-in-standby
// option.
void led_set_wifi_transfer(bool active);

// Bluetooth receiver mode: blue at full brightness. Same argument as the
// transfer above, and the same exemption from the standby option -- nothing
// plays through audio.c in that mode, so led_update_playback() reads a stopped
// player, and the LED went out twenty seconds after the screen did with a
// computer still sending music into the jack.
void led_set_bt_receiver(bool active);

// DAC mode. Nothing plays through audio.c there, so led_update_playback() sees
// a stopped player and the LED went out twenty seconds after the screen did,
// with the computer still sending music. While the host streams, the rate gets
// the colour a local track of that rate would; while the mode is merely up, the
// LED stays on the idle colour rather than being darkened by the standby
// option -- the same reasoning as the transfer above.
void led_set_dac(bool active, bool streaming, int sample_rate);

// The LED master switch. Off keeps the LED dark whatever else is
// going on -- with one exception, the same one the stock player makes: a
// charge in progress still shows its red, because a device that looks dead
// on the charger is worse than a lit LED. On (the default) restores the
// normal behaviour, including the standby option below.
void led_set_enabled(bool enabled);

// The darken-in-standby option: with it on, the LED goes dark after
// twenty seconds in standby (screen off) with nothing playing. A charge in
// progress is never darkened by it -- the red keeps breathing -- and any
// playback or wake lights the LED right back up. Re-evaluated on the same
// polls as the rest, so the switch-off lands within a few seconds.
void led_set_idle_off(bool enabled);

// Screen state, fed by power.c: the standby option only ever darkens the LED
// while the screen is off. Waking restarts the idle window.
void led_set_standby(bool standby);

// The brightness slider, 1-5, used as the level index itself -- the doc's
// `led LEVEL ...`: brightness 5 is `led 5`. Only the direct path uses it; the
// R1's plain LEDs have no such control. Saved by the caller.
void led_set_brightness(int brightness);

// The transition speed, 20-2000 ms in 10 ms steps: how long a fade-in,
// fade-out or cross-fade takes. Only the direct path uses it.
void led_set_transition_ms(int ms);

// Whether the fancy LED features (brightness, transition speed, the fades) are
// available on this device -- true on the SGM31324, false on the R1 and the
// host build. The settings page hides those rows when it is false.
bool led_effects_available(void);

// Fades the LED out on the way down. Called from power_shutdown() before the
// power goes; a no-op on the fallback path.
void led_shutdown(void);

// The same fade for a suspend to RAM: called just before the "mem" write, so
// the light is gone before the SoC sleeps, and led_resume() brings the current
// state back as a fade-in on the other side. Both are no-ops on the fallback
// path.
void led_suspend(void);
void led_resume(void);

#endif /* LED_H */
