#include "led.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "led_i2c.h"
#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"

#define PATTERN_NODE "/sys/class/leds/sgm31324-leds/led_pattern"
#define RED_TRIGGER_NODE "/sys/class/leds/red/trigger"

// The R1's blue LED (leds_pwm_add.ko, PC02, max_brightness 100). Its stock
// player writes 50 for every lit state and 0 for off.
#define BLUE_BRIGHTNESS_NODE "/sys/class/leds/blue/brightness"
#define BLUE_ON "50"

// Pattern numbers, as registered by leds_sgm31324_add.sh and used by the
// stock binary (FUN_0047ce00's logical->pattern table). See led.h for the
// full story. They are the fallback path: on the SGM31324 the controller is
// taken over and driven directly (see led_i2c.h), and these numbers are only
// written when that fails or on the R1, which has no such controller.
#define PATTERN_IDLE_AQUA 1
#define PATTERN_CHARGING_RED 6
#define PATTERN_PCM_48K 2
#define PATTERN_PCM_192K 3
#define PATTERN_PCM_HI 4
#define PATTERN_DSD_WHITE 11
#define PATTERN_OFF 5 // the logical-0 state: all registers zero
// Podcast purple: pattern 13, added by hand to the driver script by the user,
// like 12 for transfers. (The first attempt used 10, the stock MQA rendering
// pattern, which lit up magenta rather than purple.)
#define PATTERN_PODCAST_PURPLE 13
// Wi-Fi transfer: pattern 12, added by hand to the driver script
// (leds_sgm31324_add.sh, regs="000E7D00686601010706"). Lit while a transfer is
// active, and never turned off by the standby option: with the screen dark it
// is the only sign the transfer server is running.
#define PATTERN_WIFI_TRANSFER 12
// Bluetooth receiver: pattern 14, blue at full brightness, added by hand to the
// driver script like 12 and 13. Lit for as long as the mode is on, screen off
// included -- it is the only sign the player is being used as a pair of
// headphones.
#define PATTERN_BT_RECEIVER 14

// With the "LED off in standby" option on, this long without anything playing
// turns the LED off entirely.
#define IDLE_OFF_SECONDS 20

// The controller's 11-step current ladder (docs/sgm31324-led-control.md). Level
// 0 is off and each step scales a channel's 0-255 value down to 0-79.
static const int LADDER[11] = {0, 1, 3, 7, 15, 23, 31, 39, 47, 63, 79};

// The brightness slider runs 1-5 and is the level index itself, exactly as the
// doc's `led LEVEL ...` takes it: brightness 5 is `led 5`.
#define BRIGHTNESS_MIN 1
#define BRIGHTNESS_MAX 5
#define BRIGHTNESS_DEFAULT 5

// The transition speed slider runs 20 ms to 2 s in 10 ms steps; this is the
// length of a fade-in, fade-out or cross-fade.
#define TRANSITION_MIN_MS 20
#define TRANSITION_MAX_MS 2000
#define TRANSITION_DEFAULT_MS 500

// Software fades step every 10 ms, so a transition is its length in steps.
#define LED_STEP_MS 10
// A blinking state (the Wi-Fi transfer) is on and off for this long each.
#define LED_BLINK_MS 500

// The logical states the LED can be in. Each is rendered either as a driver
// pattern (the fallback) or as a colour and an effect (the direct path).
typedef enum {
	LS_OFF,
	LS_IDLE,
	LS_LOSSY, // mp3, aac, ogg, opus
	LS_CD, // lossless, 16-bit at 48 kHz or below
	LS_HIFI, // lossless above that
	LS_DSD,
	LS_PODCAST,
	LS_WIFI,
	LS_BT,
	LS_CHARGING,
	LS_CHARGED,
} led_state_t;

// How a state is played on the controller.
typedef enum {
	LM_STEADY, // a single colour, reached by a cross-fade
	LM_BREATHE, // the controller's own breathing engine
	LM_BLINK, // software on/off blink
	LM_OFF, // dark, reached by a fade-out
} led_mode_t;

typedef struct {
	led_mode_t mode;
	int r, g, b;		   // scaled 0-79
	int period, duty, fade; // breathing timing
	int transition_ms;	   // length of the fade into this state
} led_render_t;

static bool have_pattern_node;
static bool have_red_node;
static bool have_blue_node; // the R1: no pattern node, a red and a blue LED
static bool direct_mode;	// the SGM31324 is ours, driven over I2C

// Whether this player's status LED is the SGM31324 RGB controller. The model
// table knows (sysinfo.h); when system-info.json names no model we fall back to
// the driver's pattern node, which only exists where the controller does. This
// is what lets the controller be driven directly even when the firmware's
// leds_sgm31324_add.ko is never loaded at all.
static bool has_sgm31324(void) {
	const sysinfo_model_t *model = sysinfo_model();
	if (model) {
		return model->sgm31324_led;
	}
	return have_pattern_node;
}

static int current_state = -1;
static int red_trigger_state = -1; // -1 unknown, 0 "none", 1 "breathing"

static int s_brightness = BRIGHTNESS_DEFAULT;
static int s_transition_ms = TRANSITION_DEFAULT_MS;

// The three inputs the pattern is resolved from. The charger wins over
// everything -- the stock LED state machine applies its charging override
// after the playback colour -- and playback wins over idle.
static bool s_charging;
static bool s_playing;
static int s_rate;
static bool s_lossy; // the source is a lossy codec (mp3, aac, ogg, opus)
static int s_bits;	 // the source bit depth, 0 while unknown
static bool s_podcast;		 // what is playing is a podcast episode
static bool s_dsd;			 // ...or a DSD file, whatever its carrier rate
static bool s_wifi_transfer; // a Wi-Fi transfer is active
static bool s_bt_receiver;	 // Bluetooth receiver mode is on
static bool s_dac_active;	 // DAC mode is on: the gadget is up
static bool s_dac_streaming; // ...and the computer is actually sending audio
static int s_dac_rate;

static bool s_led_enabled = true; // the LED master switch
static bool s_idle_off_enabled;
static bool s_standby;		  // screen off (set by power.c)
static time_t s_standby_since; // when the screen went off
static time_t s_last_playing; // when something was last playing
static bool s_on_charger;	  // a cable is supplying, charge finished or not
static time_t s_charger_since; // when that last changed

// --- the effect thread -----------------------------------------------------
//
// The fades are CPU-driven walks of the controller's levels, so they cannot run
// on the GUI thread. One worker owns the I2C bus and plays whatever target the
// state machine last posted; a new target cuts a running fade short rather than
// queueing behind it, which is what makes a colour change feel immediate.

static pthread_t led_thread;
static pthread_mutex_t led_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t led_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t led_done_cond = PTHREAD_COND_INITIALIZER;
static led_render_t led_target;
static volatile unsigned led_seq;
static unsigned led_done_seq; // the target seq the worker has finished (under led_lock)
static bool led_thread_started;

static void write_str(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return;
	}
	fputs(value, f);
	fclose(f);
}

static int led_level(void) {
	int b = s_brightness;
	if (b < BRIGHTNESS_MIN) {
		b = BRIGHTNESS_MIN;
	}
	if (b > BRIGHTNESS_MAX) {
		b = BRIGHTNESS_MAX;
	}
	return b;
}

// Scales a 0-255 channel through the level ladder. White is run 40% dimmer --
// the white LED reads brighter than the coloured ones -- but a channel that was
// lit is never dimmed all the way to zero, which would put the light out rather
// than dim it.
static void scale_rgb(int r, int g, int b, bool white, int *out_r, int *out_g, int *out_b) {
	int c = LADDER[led_level()];
	int sr = r * c / 255;
	int sg = g * c / 255;
	int sb = b * c / 255;
	if (white) {
		if (sr > 0) {
			sr = sr * 60 / 100;
			if (sr < 1) {
				sr = 1;
			}
		}
		if (sg > 0) {
			sg = sg * 60 / 100;
			if (sg < 1) {
				sg = 1;
			}
		}
		if (sb > 0) {
			sb = sb * 60 / 100;
			if (sb < 1) {
				sb = 1;
			}
		}
	}
	*out_r = sr;
	*out_g = sg;
	*out_b = sb;
}

static unsigned ch_mask(int r, int g, int b) {
	unsigned m = 0;
	if (r > 0) {
		m |= LED_CH_R;
	}
	if (g > 0) {
		m |= LED_CH_G;
	}
	if (b > 0) {
		m |= LED_CH_B;
	}
	return m;
}

static void led_post(const led_render_t *t) {
	pthread_mutex_lock(&led_lock);
	led_target = *t;
	led_seq++;
	pthread_cond_signal(&led_cond);
	pthread_mutex_unlock(&led_lock);
}

// Sleeps `ms` in small slices, returning true if a newer target arrived while it
// waited -- which is what lets a running fade be cut short by a colour change
// instead of being played out to the end first.
static bool led_wait(unsigned seen, int ms) {
	int left = ms;
	while (left > 0) {
		int slice = left > 5 ? 5 : left;
		struct timespec ts = {0, (long)slice * 1000000L};
		nanosleep(&ts, NULL);
		if (led_seq != seen) {
			return true;
		}
		left -= slice;
	}
	return false;
}

static void *led_worker(void *arg) {
	(void)arg;
	int cur_r = 0, cur_g = 0, cur_b = 0;
	unsigned cur_ch = 0;
	unsigned seen = 0;

	for (;;) {
		pthread_mutex_lock(&led_lock);
		while (led_seq == seen) {
			pthread_cond_wait(&led_cond, &led_lock);
		}
		led_render_t t = led_target;
		seen = led_seq;
		pthread_mutex_unlock(&led_lock);

		if (led_i2c_ready()) {
			if (t.mode == LM_BREATHE) {
				led_i2c_breathe(t.r, t.g, t.b, (unsigned char)t.period, (unsigned char)t.duty, (unsigned char)t.fade);
				cur_r = t.r;
				cur_g = t.g;
				cur_b = t.b;
				cur_ch = ch_mask(t.r, t.g, t.b);
			} else if (t.mode == LM_BLINK) {
				unsigned ch = ch_mask(t.r, t.g, t.b);
				led_i2c_steady();
				for (;;) {
					led_i2c_levels(t.r, t.g, t.b, ch);
					if (led_wait(seen, LED_BLINK_MS)) {
						break;
					}
					led_i2c_levels(0, 0, 0, ch);
					if (led_wait(seen, LED_BLINK_MS)) {
						break;
					}
				}
				cur_r = 0;
				cur_g = 0;
				cur_b = 0;
				cur_ch = ch;
			} else {
				// Steady or off: walk from whatever is on now to the target. From
				// black this is a fade-in, to black a fade-out, and between two
				// colours a cross-fade -- the same walk, only the endpoints differ.
				// Every channel lit at either end stays enabled for the duration,
				// so a channel that is dark at one end fades rather than snapping.
				int tr = (t.mode == LM_OFF) ? 0 : t.r;
				int tg = (t.mode == LM_OFF) ? 0 : t.g;
				int tb = (t.mode == LM_OFF) ? 0 : t.b;
				unsigned tch = (t.mode == LM_OFF) ? 0 : ch_mask(tr, tg, tb);
				unsigned ramp_ch = cur_ch | tch;
				int steps = t.transition_ms / LED_STEP_MS;
				if (steps < 1) {
					steps = 1;
				}

				int sr = cur_r, sg = cur_g, sb = cur_b;
				led_i2c_steady();
				bool cut = false;
				for (int i = 1; i <= steps; i++) {
					int r, g, b;
					if (t.mode == LM_OFF) {
						// The doc's fade_out: a quadratic fall, so it eases out
						// rather than dropping in a straight line.
						int k = steps - i;
						r = sr * k * k / (steps * steps);
						g = sg * k * k / (steps * steps);
						b = sb * k * k / (steps * steps);
					} else {
						r = sr + (tr - sr) * i / steps;
						g = sg + (tg - sg) * i / steps;
						b = sb + (tb - sb) * i / steps;
					}
					led_i2c_levels(r, g, b, ramp_ch);
					cur_r = r;
					cur_g = g;
					cur_b = b;
					if (i < steps && led_wait(seen, LED_STEP_MS)) {
						cut = true;
						break;
					}
				}
				if (cut) {
					cur_ch = ramp_ch;
				} else {
					if (t.mode == LM_OFF) {
						// The map off entirely, as the doc's fade_out leaves it.
						led_i2c_off();
					}
					cur_r = tr;
					cur_g = tg;
					cur_b = tb;
					cur_ch = tch;
				}
			}
		}

		// This target is done (or was cut short by a newer one). Whoever is
		// waiting for a fade to finish -- the shutdown path -- is told here.
		pthread_mutex_lock(&led_lock);
		led_done_seq = seen;
		pthread_cond_broadcast(&led_done_cond);
		pthread_mutex_unlock(&led_lock);
	}
	return NULL;
}

static void start_worker(void) {
	if (led_thread_started) {
		return;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&led_thread, &attr, led_worker, NULL) == 0) {
		led_thread_started = true;
	} else {
		fprintf(stderr, "led: could not start the effect thread\n");
	}
	pthread_attr_destroy(&attr);
}

// --- state -> output -------------------------------------------------------

// The quality tier of what is playing, which is what the colour says: the same
// four the library badges use. Keying on the tier rather than the bare sample
// rate is what tells an MP3 and a FLAC of the same rate apart. A DSD file is
// told apart by its own flag (see led_update_playback): over DoP its rate is
// that of the PCM carrying it.
static led_state_t quality_state(int sample_rate, bool lossy, int bits, bool dsd) {
	if (dsd || sample_rate >= 2822400) {
		return LS_DSD;
	}
	if (lossy) {
		return LS_LOSSY;
	}
	if (sample_rate > 48000 || bits > 16) {
		return LS_HIFI;
	}
	return LS_CD;
}

// The driver pattern a state falls back to. There is no green pattern
// registered, so a fully charged battery shows the idle aqua here; the direct
// path below paints it green.
static int state_pattern(led_state_t s) {
	switch (s) {
	case LS_OFF:
		return PATTERN_OFF;
	case LS_IDLE:
		return PATTERN_IDLE_AQUA;
	case LS_LOSSY:
		return PATTERN_PCM_48K;
	case LS_CD:
		return PATTERN_PCM_192K;
	case LS_HIFI:
		return PATTERN_PCM_HI;
	case LS_DSD:
		return PATTERN_DSD_WHITE;
	case LS_PODCAST:
		return PATTERN_PODCAST_PURPLE;
	case LS_WIFI:
		return PATTERN_WIFI_TRANSFER;
	case LS_BT:
		return PATTERN_BT_RECEIVER;
	case LS_CHARGING:
		return PATTERN_CHARGING_RED;
	case LS_CHARGED:
		return PATTERN_IDLE_AQUA;
	}
	return PATTERN_OFF;
}

// The colour and effect a state is played with on the direct path. The colours
// are the ones the registered patterns stand for: aqua idle, yellow 48 kHz,
// aqua up to 192 kHz, orange above it, white DSD, purple podcasts, blue the
// Bluetooth receiver, red breathing while charging and green once charged.
static void build_render(led_state_t s, led_render_t *out) {
	int r = 0, g = 0, b = 0;
	bool white = false;
	led_mode_t mode = LM_STEADY;
	switch (s) {
	case LS_OFF:
		mode = LM_OFF;
		break;
	case LS_IDLE:
	case LS_CD:
		g = 255;
		b = 255;
		break;
	case LS_LOSSY:
		r = 255;
		g = 255;
		break;
	case LS_HIFI:
		r = 255;
		g = 128;
		break;
	case LS_DSD:
		r = 255;
		g = 255;
		b = 255;
		white = true;
		break;
	case LS_PODCAST:
		r = 160;
		b = 255;
		break;
	case LS_WIFI:
		g = 128;
		b = 255;
		mode = LM_BLINK;
		break;
	case LS_BT:
		b = 255;
		break;
	case LS_CHARGED:
		g = 255;
		break;
	case LS_CHARGING:
		r = 255;
		mode = LM_BREATHE;
		break;
	}
	scale_rgb(r, g, b, white, &out->r, &out->g, &out->b);
	out->mode = mode;
	out->period = 0x1E; // the stock charging pulse
	out->duty = 0x7D;
	out->fade = 0xBB;
	out->transition_ms = s_transition_ms;
}

// Renders a state to whichever output is available: the direct controller, the
// driver's pattern node, or the R1's plain blue LED. Cheap to call repeatedly:
// it only writes when the state changes.
static void render_state(led_state_t state) {
	if ((int)state == current_state) {
		return;
	}
	if (direct_mode) {
		led_render_t t;
		build_render(state, &t);
		led_post(&t);
	} else if (have_pattern_node) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", state_pattern(state));
		write_str(PATTERN_NODE, buf);
	} else if (have_blue_node) {
		// On the R1 the pattern only decides whether the blue LED is lit: off
		// and the charging red leave it dark, every colour becomes blue.
		bool blue = state != LS_OFF && state != LS_CHARGING;
		write_str(BLUE_BRIGHTNESS_NODE, blue ? BLUE_ON : "0");
	} else {
		return;
	}
	current_state = state;
}

// When the "off in standby" countdown starts: the latest of the screen going
// dark, the last moment something was playing, and the cable going in or out.
static time_t idle_window_start(void) {
	time_t latest = s_standby_since > s_last_playing ? s_standby_since : s_last_playing;
	return s_charger_since > latest ? s_charger_since : latest;
}

static void apply(void) {
	// s_charging is "a charge is going in", not "a cable is in": the caller
	// takes the battery being full, or held at its limit, out of it, and hands
	// this a settled answer rather than a reading that is still wobbling. See
	// led.h.
	bool charging_now = s_charging;

	// The separate red classdev: the R1's red LED. Breathing while a charge goes
	// in, released otherwise. The R3 Pro II has no such node, and its charging
	// red comes from the state below.
	int want_red = charging_now ? 1 : 0;
	if (have_red_node && want_red != red_trigger_state) {
		red_trigger_state = want_red;
		write_str(RED_TRIGGER_NODE, charging_now ? "breathing" : "none");
	}

	led_state_t state;
	if (!s_led_enabled) {
		// The master switch is off: the LED stays dark, except that a charge in
		// progress still shows -- a device that looks dead while it is charging
		// is worse than a lit LED.
		state = charging_now ? LS_CHARGING : LS_OFF;
	} else if (charging_now) {
		// Charging wins over everything, the stock priority: the red breathes
		// while a charge goes in.
		state = LS_CHARGING;
	} else if (s_on_charger) {
		// A cable is supplying and the charge is done -- full, or held at the
		// configured limit. Green says so, and like the red it is never darkened
		// by the standby option: a player left plugged in overnight stays lit.
		state = LS_CHARGED;
	} else if (s_playing && s_rate > 0) {
		// Playback always shows its colour. A podcast has its own colour, purple,
		// whatever the sample rate: it says what is being listened to, not how it
		// is sampled.
		state = s_podcast ? LS_PODCAST : quality_state(s_rate, s_lossy, s_bits, s_dsd);
	} else if (s_dac_streaming && s_dac_rate > 0) {
		// In DAC mode nothing plays through audio.c, so s_playing is false and
		// the LED went dark twenty seconds after the screen did -- with the
		// computer still sending music. The rate the host chose gets the same
		// colour a local track of that rate would; a USB host sends PCM, so it is
		// never lossy.
		state = quality_state(s_dac_rate, false, 0, false);
	} else if (s_wifi_transfer) {
		// A Wi-Fi transfer is running: its light stays lit even with the screen
		// off, since it is the only sign the server is running, and it
		// deliberately overrides the off-in-standby option.
		state = LS_WIFI;
	} else if (s_bt_receiver) {
		// Bluetooth receiver mode, lit whether or not the sender is sending
		// right now: the mode is what the LED reports, the way the transfer
		// above reports its server. Nothing plays through audio.c here, so the
		// branch at the top never fires and without this one the LED went dark
		// twenty seconds after the screen did -- with music still coming out of
		// the jack.
		state = LS_BT;
	} else if (s_dac_active) {
		// The mode is up but the computer is not sending anything yet. Same
		// argument as the transfer above: with the screen dark the LED is the
		// only sign the player is a sound card, so the standby option does not
		// reach it.
		state = LS_IDLE;
	} else if (s_idle_off_enabled && s_standby && time(NULL) - idle_window_start() >= IDLE_OFF_SECONDS) {
		// LED off in standby: the screen has been off this long with nothing
		// playing and nothing else going on. The window runs from whichever came
		// last -- the screen going dark, playback stopping, the cable going in or
		// out. Counted from the screen alone, a track paused with the screen
		// already off would find the window long since elapsed and put the LED
		// out the instant the music stopped, with no aqua in between. With the
		// screen on the aqua stays: the device is visibly in use.
		//
		// A cable is handled above (LS_CHARGED), which is the point: a battery
		// that reaches 100%, or its configured limit, has nothing left to say in
		// red, and a player left plugged in overnight would otherwise answer the
		// end of its charge by going dark.
		state = LS_OFF;
	} else {
		state = LS_IDLE;
	}
	render_state(state);
}

void led_init(void) {
	have_pattern_node = access(PATTERN_NODE, W_OK) == 0;
	have_red_node = access(RED_TRIGGER_NODE, W_OK) == 0;
	have_blue_node = !have_pattern_node && access(BLUE_BRIGHTNESS_NODE, W_OK) == 0;

	bool sgm = has_sgm31324();
	if (!sgm && !have_red_node && !have_blue_node) {
		return; // host build, or firmware without the LED module
	}

	// The switches and the two effect controls, as they were left, so the very
	// first render already honours them.
	s_led_enabled = config_get_bool("power", "led_on", true);
	s_idle_off_enabled = config_get_bool("power", "led_off_standby", true);
	s_brightness = (int)config_get_int("power", "led_brightness", BRIGHTNESS_DEFAULT);
	s_transition_ms = (int)config_get_int("power", "led_transition_ms", TRANSITION_DEFAULT_MS);

	// On the SGM31324 (the V1 and the R3 Pro II) drive the controller directly
	// -- that is what the fade-in below and the colour cross-fades need. The
	// controller is free either because the firmware driver was never loaded (the
	// packer disables it) or because led_i2c_init() unloads it. The R1 has no
	// such controller and keeps its pattern/blue path.
	if (sgm) {
		direct_mode = led_i2c_init();
	}
	if (direct_mode) {
		start_worker();
	}

	const char *mode = direct_mode ? "direct i2c"
					   : have_pattern_node ? "pattern node"
					   : have_blue_node	 ? "blue pwm"
										 : "unavailable";
	fprintf(stderr, "led: %s, red trigger %s, blue %s\n", mode, have_red_node ? "ok" : "missing",
			have_blue_node ? "ok" : "missing");

	s_last_playing = time(NULL); // the power-on colour holds for the first idle window

	// Exactly the stock startup sequence: red trigger released, then the idle
	// colour. On the direct path the worker starts from black, so this first
	// render is a fade-in.
	if (have_red_node) {
		write_str(RED_TRIGGER_NODE, "none");
		red_trigger_state = 0;
	}
	apply();
}

void led_set_charging(bool charging) {
	s_charging = charging;
	apply(); // cheap: both writes only happen on a change
}

void led_set_on_charger(bool present) {
	if (present == s_on_charger) {
		return;
	}
	s_on_charger = present;
	// A cable going in or out restarts the idle window, so the aqua gets its
	// twenty seconds after an unplug instead of the LED going dark in the same
	// instant as the cable leaves.
	s_charger_since = time(NULL);
	apply();
}

void led_update_playback(bool playing, int sample_rate, bool lossy, int bits, bool podcast, bool dsd) {
	s_playing = playing && sample_rate > 0;
	s_rate = sample_rate;
	s_lossy = lossy;
	s_bits = bits;
	s_podcast = podcast;
	s_dsd = dsd;
	if (s_playing) {
		s_last_playing = time(NULL);
	}
	apply();
}

void led_set_dac(bool active, bool streaming, int sample_rate) {
	if (active == s_dac_active && streaming == s_dac_streaming && sample_rate == s_dac_rate) {
		return;
	}
	s_dac_active = active;
	s_dac_streaming = streaming;
	s_dac_rate = sample_rate;
	apply();
}

void led_set_wifi_transfer(bool active) {
	if (active == s_wifi_transfer) {
		return;
	}
	s_wifi_transfer = active;
	apply();
}

void led_set_bt_receiver(bool active) {
	if (active == s_bt_receiver) {
		return;
	}
	s_bt_receiver = active;
	apply();
}

void led_set_enabled(bool enabled) {
	s_led_enabled = enabled;
	apply();
}

void led_set_idle_off(bool enabled) {
	s_idle_off_enabled = enabled;
	apply();
}

void led_set_standby(bool standby) {
	if (standby && !s_standby) {
		s_standby_since = time(NULL); // the idle window starts here
	}
	s_standby = standby;
	if (!standby) {
		// Waking counts as activity: the aqua shows for a fresh idle window
		// before the option can dim it again.
		s_last_playing = time(NULL);
		s_standby_since = time(NULL);
	}
	apply();
}

void led_set_brightness(int brightness) {
	if (brightness < BRIGHTNESS_MIN) {
		brightness = BRIGHTNESS_MIN;
	}
	if (brightness > BRIGHTNESS_MAX) {
		brightness = BRIGHTNESS_MAX;
	}
	if (brightness == s_brightness) {
		return;
	}
	s_brightness = brightness;
	if (direct_mode) {
		current_state = -1; // force a re-render at the new level
		apply();
	}
}

void led_set_transition_ms(int ms) {
	if (ms < TRANSITION_MIN_MS) {
		ms = TRANSITION_MIN_MS;
	}
	if (ms > TRANSITION_MAX_MS) {
		ms = TRANSITION_MAX_MS;
	}
	s_transition_ms = ms;
}

bool led_effects_available(void) {
	// The direct path is available once we own the controller, or -- before
	// led_init() has run -- on a model whose LED is the SGM31324. The R1 has
	// neither.
	if (direct_mode) {
		return true;
	}
	const sysinfo_model_t *model = sysinfo_model();
	if (model) {
		return model->sgm31324_led;
	}
	return access(PATTERN_NODE, F_OK) == 0;
}

// Posts a fade to black and waits for the worker to actually finish it, rather
// than sleeping for the nominal duration: the I2C writes make a fade take a
// little longer than its length in steps, and a fixed sleep cut the last of it
// off -- the light snapped out instead of reaching black. Shared by the
// power-off and the suspend paths, both of which want the light gone before the
// power does.
static void fade_out_and_wait(void) {
	if (!direct_mode) {
		return;
	}

	pthread_mutex_lock(&led_lock);
	memset(&led_target, 0, sizeof(led_target));
	led_target.mode = LM_OFF;
	led_target.transition_ms = s_transition_ms;
	led_seq++;
	unsigned want = led_seq;
	pthread_cond_signal(&led_cond);

	// A safety net, not the expected path: the longest fade is two seconds.
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	while (led_done_seq < want) {
		if (pthread_cond_timedwait(&led_done_cond, &led_lock, &deadline) != 0) {
			break;
		}
	}
	pthread_mutex_unlock(&led_lock);
}

void led_shutdown(void) { fade_out_and_wait(); }

void led_suspend(void) { fade_out_and_wait(); }

void led_resume(void) {
	// The light was faded out for the sleep; bring the current state back, as a
	// fade-in from black. Forcing the render is what does it -- the state itself
	// has not changed, so render_state() would otherwise skip it.
	if (!direct_mode) {
		return;
	}
	current_state = -1;
	apply();
}
