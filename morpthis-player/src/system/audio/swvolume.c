#include "swvolume.h"

#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// SW/HDB, the one software curve the R3 Pro II's ot_devices.json defines, in
// tenths of a dB. Index 0 is -150 dB and not silence: it is far below anything
// a converter can render, but it is not a zero coefficient, so a real mute is
// handled separately below.
//
// The shape is deliberate -- huge steps at the bottom, 0.3 dB steps at the top,
// so the last few points of the scale are fine adjustment rather than the
// blunt 1% of a linear percentage.
static const int SW_HDB[101] = {
	-1500,													  //
	-1200, -600, -510, -470, -420, -395, -375, -361, -358, -346, //
	-335,  -325, -316, -308, -300, -292, -285, -278, -271, -264, //
	-258,  -252, -246, -240, -235, -230, -225, -220, -215, -210, //
	-207,  -204, -201, -198, -195, -192, -189, -186, -183, -180, //
	-177,  -174, -171, -168, -165, -162, -159, -156, -153, -150, //
	-147,  -144, -141, -138, -135, -132, -129, -126, -123, -120, //
	-117,  -114, -111, -108, -105, -102, -99,  -96,  -93,  -90,  //
	-87,   -84,  -81,  -78,  -75,  -72,  -69,  -66,  -63,  -60,  //
	-57,   -54,  -51,  -48,  -45,  -42,  -39,  -36,  -33,  -30,  //
	-27,   -24,  -21,  -18,  -15,  -12,  -9,   -6,   -3,   0};

// The coefficients, worked out once. pow() is a poor thing to call per sample
// and an unnecessary one to call per volume change: there are 101 answers and
// they never change.
static int64_t coefficient[101];
static bool table_ready;

// The index in force. A plain int, written by whoever moves the volume and read
// by the playback thread; a word-sized load and store, so the worst a reader
// can see is the level from a moment ago.
static int sw_index = 100;

static int clamp_index(int percent) {
	if (percent < 0) {
		return 0;
	}
	if (percent > 100) {
		return 100;
	}
	return percent;
}

// The stock binary's own arithmetic, down to the intermediate type. The
// division is done in float and only the result is widened for pow(): dividing
// in double instead moves about ninety of the 101 coefficients by up to 35
// counts, which is inaudible but is not the same number.
static int64_t stock_q31(int effective_tenths) {
	float exponent = (float)effective_tenths / 200.0f;
	return (int64_t)(pow(10.0, (double)exponent) * 2147483648.0);
}

static void build_table(void) {
	if (table_ready) {
		return;
	}
	for (int i = 0; i <= 100; i++) {
		coefficient[i] = stock_q31(SW_HDB[i]);
	}
	// Index 0 is silence rather than the -150 dB the curve carries. The stock
	// engine sets both channel coefficients to zero there and the samples go
	// out as a zeroed buffer; -150 dB leaves a coefficient of 67, which is a
	// signal, and the one place a volume control must be exact is the bottom.
	coefficient[0] = 0;
	table_ready = true;
}

void swvolume_set_index(int percent) {
	build_table();
	sw_index = clamp_index(percent);
}

int swvolume_millibel(int percent) { return SW_HDB[clamp_index(percent)]; }

int64_t swvolume_coefficient(int percent) {
	build_table();
	return coefficient[clamp_index(percent)];
}

// The one route this curve belongs to: a device on the USB-C port with no
// volume control of its own. There the CS43198 is not in the path at all, so
// the volume keys have nothing else to move -- the stream is the only place
// left to put the level.
//
// Everywhere else something better holds it. On the jacks it is the converter's
// own attenuation, which is the whole volume law there and needs no partner:
// each hardware table steps 0.5 dB per index from 100 down to 45, 1 dB down to
// 10, then 2 and 5, and MDB sits exactly 6.0 dB under HDB at every index.
// Over Bluetooth the headphones hold it (A2DP_FIXED_GAIN in the stock
// configuration, and AVRCP absolute volume here). On a USB device that
// publishes a control of its own, that control is written directly.
//
// Bluetooth is asked first and separately: usbaudio_poll() gives up as soon as
// Bluetooth is playing, so a USB card that was in use keeps saying so while the
// sound is going over the air.
// The gain usbaudio.c asked for, as a Q31 coefficient; 0 is silence. Written
// whole by the thread that moves the volume and read by the playback thread,
// the same word-sized handoff as sw_index.
static volatile int64_t usb_gain_q31 = 2147483648LL;
static volatile bool usb_gain_set;

void swvolume_set_usb_gain(long db100, bool mute) {
	if (db100 > 0) {
		db100 = 0;
	}
	usb_gain_q31 = mute ? 0 : (int64_t)(pow(10.0, (double)db100 / 2000.0) * 2147483648.0);
	usb_gain_set = true;
}

void swvolume_clear_usb_gain(void) {
	usb_gain_set = false;
	usb_gain_q31 = 2147483648LL;
}

bool swvolume_active(void) {
	if (audio_output_is_bluetooth()) {
		return false;
	}
	if (!usbaudio_active()) {
		return false;
	}
	if (usb_gain_set) {
		return usb_gain_q31 < 2147483648LL;
	}
	return !usbaudio_has_volume_control();
}

// What to do with this buffer: the Q31 coefficient, or one of two answers that
// are not a multiply.
typedef enum {
	SW_PASS,  // leave the samples alone
	SW_MUTE,  // fill with silence
	SW_SCALE, // multiply by `gain`
} sw_action_t;

static sw_action_t gain_now(int64_t *gain) {
	*gain = 0;
	// Built here as well as from the setters: the very first buffers can
	// arrive before anybody has walked the index curve, and a volume that
	// waits for a side effect to be remembered is a volume that can fail to.
	build_table();
	if (!swvolume_active()) {
		return SW_PASS;
	}

	if (usb_gain_set) {
		int64_t q31 = usb_gain_q31;
		if (q31 == 0) {
			return SW_MUTE;
		}
		if (q31 >= 2147483648LL) {
			return SW_PASS;
		}
		*gain = q31;
		return SW_SCALE;
	}

	int idx = clamp_index(sw_index);
	if (idx == 0) {
		return SW_MUTE;
	}

	int64_t q31 = coefficient[idx];
	if (q31 == 2147483648LL) {
		return SW_PASS; // unity: index 100
	}

	static int said = -1;
	if (said != idx) {
		said = idx;
		fprintf(stderr, "swvolume: index %d -> %.1f dB (Q31 %lld)\n", idx, SW_HDB[idx] / 10.0, (long long)q31);
	}

	*gain = q31;
	return SW_SCALE;
}

// How a sample is scaled. The product is Q31; the answer sits at the top of it.
//
// Truncating is what the stock binary does to its own curve, and this curve
// was first reproduced sample for sample. But on this route -- the whole
// volume law for a USB DAC with no control of its own -- truncation is heard
// for what it is: past about -50 dB the error left behind is correlated with
// the music, and the bottom of the scale becomes grit, garble and whistling
// where the level should only be quiet. So the products are rounded to
// nearest, and on the low-depth outputs (16-bit and 8-bit) a TPDF dither of
// one LSB either side is added before the shift, which replaces that
// distortion with ordinary hiss underneath the music. The 24- and 32-bit
// paths are deep enough that rounding alone leaves nothing to hear.
//
// The dither is the difference of two uniform draws -- a triangle spanning
// one LSB either side of zero. One xorshift state serves whichever threads
// apply the volume; zero is a fixed point of xorshift and the state is seeded
// non-zero, so it can never die, and the worst a race between two threads can
// do is make them draw matching sequences for one sample. Noise uncorrelated
// with the signal either way.
static uint32_t dither_state = 0x2545F491u;

static inline int32_t dither_uniform(void) {
	uint32_t x = dither_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	dither_state = x;
	return (int32_t)(x & 0x7FFFFFFFu);
}

// `dither` true: TPDF noise spanning one LSB either side, then round to
// nearest -- the error is then independent of the signal and unbiased, which
// is what turns deep attenuation into hiss rather than grit. `dither` false:
// round only.
static inline int32_t swvolume_scale(int32_t sample, int64_t gain, bool dither) {
	int64_t product = (int64_t)sample * gain;
	product += (int64_t)1 << 30; // round to nearest
	if (dither) {
		product += (int64_t)dither_uniform() - (int64_t)dither_uniform();
	}
	return (int32_t)(product >> 31);
}

void swvolume_apply_u8(unsigned char *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		// Not zero: unsigned 8-bit PCM has silence at the middle of its range.
		memset(samples, 0x80, (size_t)count);
		return;
	}
	for (int i = 0; i < count; i++) {
		int sample = (int)samples[i] - 128;
		int scaled = swvolume_scale(sample, gain, true);
		// Clamped and not cast: at a gain close to unity the dither can carry
		// a full-scale sample one LSB past the end of the range, and a wrap
		// there is a click, not a rounding.
		if (scaled < -128) {
			scaled = -128;
		} else if (scaled > 127) {
			scaled = 127;
		}
		samples[i] = (unsigned char)(scaled + 128);
	}
}

void swvolume_apply_s16(short *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * sizeof(*samples));
		return;
	}
	for (int i = 0; i < count; i++) {
		int32_t scaled = swvolume_scale(samples[i], gain, true);
		// As above: dithered output clamps instead of wrapping.
		if (scaled < -32768) {
			scaled = -32768;
		} else if (scaled > 32767) {
			scaled = 32767;
		}
		samples[i] = (short)scaled;
	}
}

// S24_3LE: three bytes per sample, little-endian, signed, and nothing between
// one sample and the next. The sample has to be sign-extended out of 24 bits
// before the multiply and only three bytes written back -- the byte after the
// last sample belongs to the next frame, or to whatever follows the PCM in the
// buffer.
void swvolume_apply_s24_3le(unsigned char *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * 3);
		return;
	}
	for (int i = 0; i < count; i++) {
		unsigned char *p = samples + (size_t)i * 3;
		int32_t sample = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
		if (sample & 0x800000) {
			sample -= 0x1000000;
		}
		int32_t scaled = swvolume_scale(sample, gain, false);
		p[0] = (unsigned char)(scaled & 0xFF);
		p[1] = (unsigned char)((scaled >> 8) & 0xFF);
		p[2] = (unsigned char)((scaled >> 16) & 0xFF);
	}
}

void swvolume_apply_s32(int32_t *samples, int count) {
	int64_t gain = 0;
	sw_action_t action = gain_now(&gain);
	if (action == SW_PASS) {
		return;
	}
	if (action == SW_MUTE) {
		memset(samples, 0, (size_t)count * sizeof(*samples));
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = swvolume_scale(samples[i], gain, false);
	}
}
