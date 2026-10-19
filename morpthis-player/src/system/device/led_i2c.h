#ifndef LED_I2C_H
#define LED_I2C_H

#include <stdbool.h>

// Direct register control of the SGM31324 RGB LED controller on I2C bus 0
// (address 0x30) -- the userspace path documented in
// docs/sgm31324-led-control.md.
//
// The firmware's leds_sgm31324_add.ko owns the controller and exposes a table
// of preset patterns; led_i2c_init() unloads it so these routines can program
// the registers themselves. The values passed here are the controller's own
// 0-79 current levels (the level ladder in the doc), already scaled by the
// caller -- this layer knows nothing about colours or brightness.
//
// Everything degrades to a no-op when the bus cannot be opened, so the caller
// can fall back to the driver's patterns.

// Frees the controller (rmmod leds_sgm31324_add) and opens the I2C bus.
// Returns false when the bus cannot be opened, leaving the driver in place.
bool led_i2c_init(void);

// True once led_i2c_init() has succeeded.
bool led_i2c_ready(void);

// The steady-mode channel bits, as they go into the enable map (register 0x04).
#define LED_CH_R 0x01u
#define LED_CH_G 0x04u
#define LED_CH_B 0x10u

// Steady colour. r, g, b are 0-79; `channels` is the bit mask of channels to
// enable (LED_CH_R/G/B), so a channel held at 0 can still be enabled for a
// cross-fade. Writes the enable map and the three levels.
void led_i2c_levels(int r, int g, int b, unsigned channels);

// Puts the controller in steady mode (register 0x09 = 0x00).
void led_i2c_steady(void);

// Hardware breathing: the controller's own engine, so the pulse keeps running
// with no CPU involvement. r, g, b are 0-79; a channel at 0 is left out of the
// enable map. period, duty and fade are the raw register values (the stock
// charging pulse is 0x1E, 0x7D, 0xBB).
void led_i2c_breathe(int r, int g, int b, unsigned char period, unsigned char duty, unsigned char fade);

// Everything off: the enable map keeps only DEV_EN and the levels go to zero.
void led_i2c_off(void);

#endif /* LED_I2C_H */
