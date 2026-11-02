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

// ---------------------------------------------------------------------------
// The kernel engine (morpthis_led.ko, kernel/morpthis_led/ in this repository)
//
// The same controller and the same effects, run by the kernel instead of the
// LED worker thread: a fade is ten-millisecond kernel work, unchanged
// registers are not re-sent, the blink wakes no process, and the light goes
// out at any reboot or power-off. led_i2c_init() loads it when the image
// carries it ([led] kernel_driver = 0 turns that off) and the I2C routines
// above stay the fallback: a module that will not load, or a previous boot
// that never got past loading it, leaves the player on its own I2C path.
// ---------------------------------------------------------------------------

// True when the kernel engine is driving the light; the led_i2c_* routines
// above are then not used.
bool led_kmod_ready(void);

// The modes, as the module's `render` node takes them.
#define LED_KMOD_STEADY 0
#define LED_KMOD_BREATHE 1
#define LED_KMOD_BLINK 2
#define LED_KMOD_OFF 3

// Hands one target to the engine, which cuts whatever it is running from
// wherever it had got to -- the same semantics as the worker's own walk.
bool led_kmod_render(int mode, int r, int g, int b, int period, int duty, int fade, int transition_ms,
					 int step_ms);

// Waits a short while (50 ms at most) for a running fade to end. 1 while it
// is still running, 0 once the engine is idle, -1 if the node cannot be read.
int led_kmod_wait(void);

#endif /* LED_I2C_H */
