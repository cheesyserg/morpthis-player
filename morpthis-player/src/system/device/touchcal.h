#ifndef TOUCHCAL_H
#define TOUCHCAL_H

#include <stdbool.h>
#include <stdint.h>

// The touch panel's seam, as runtime values.
//
// The panel holds its Y coordinate across a short band and then releases it
// with a jump. main.c corrects that per gesture; the values it corrects with
// were measured once and compiled in. They live here now, so the calibration
// page under Developer options can measure them on the device and keep the
// result across a reboot in device_config.ini.
//
// Everything is in unrotated controller coordinates. The seam code mirrors the
// values when the screen is turned over, so a calibration made in either
// orientation means the same thing.

void touchcal_init(void); // idempotent; every getter calls it

int32_t touchcal_low(void);     // the held value below the band
int32_t touchcal_high(void);    // the held value above it
int32_t touchcal_gap(void);     // their distance, the width to fold
int32_t touchcal_band_lo(void); // inner edge used for crossing detection
int32_t touchcal_band_hi(void);
int32_t touchcal_min_step(void); // smallest step treated as a crossing
int32_t touchcal_tap_snap(void); // how near a held value counts for a tap

// ---------------------------------------------------------------------------
// A calibration session, driven by the Developer options page.
//
// `extent` is the last row of the panel (319 on the V1). Only steps that cross
// the middle row are accepted: that is where the seam is, and the turn at the
// edge of a swipe stays on one side of it.
// ---------------------------------------------------------------------------

void touchcal_begin(int32_t extent);
void touchcal_end(void);
bool touchcal_active(void);

// One raw, unrotated panel coordinate per touch read. `pressed` is the indev
// state; a release forgets the previous point so the next press starts clean.
void touchcal_feed(int32_t y, bool pressed);

// How many crossings the strongest cluster holds. Both directions count.
uint32_t touchcal_samples(void);

// How many it needs before touchcal_result() can succeed.
uint32_t touchcal_needed(void);

// The measured plateaus and their distance. False while the strongest cluster
// is too small or its values are not a plausible band for this panel.
bool touchcal_result(int32_t *low, int32_t *high, int32_t *gap);

// Applies a measurement and writes it to device_config.ini. False when the
// values are not a plausible band. touchcal_defaults() restores the built-in
// values (and writes those).
bool touchcal_apply(int32_t low, int32_t high);
void touchcal_defaults(void);

#endif /* TOUCHCAL_H */
