// test_swvolume.c -- the software volume's arithmetic, on the host.
//
// The whole volume law for a USB DAC without a control of its own lives in
// swvolume.c, and the two properties that matter are easy to get wrong: the
// gain must mean exactly the decibels usbaudio.c asked for, and a deeply
// attenuated stream must come out quiet and clean -- rounded and dithered --
// rather than wrapped, grubby or garbled by truncation. Both are checked here
// against the real implementation; only the three routing answers (Bluetooth,
// a card on the port, that card's volume control) are stubbed.

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/swvolume.h"

// The routing answers swvolume.c asks for. Settable so a test can stand where
// the port would.
static bool stub_bluetooth_active = false;
static bool stub_usbaudio_active = true;
static bool stub_usbaudio_has_volume_control = false;

bool audio_output_is_bluetooth(void) { return stub_bluetooth_active; }
bool usbaudio_active(void) { return stub_usbaudio_active; }
bool usbaudio_has_volume_control(void) { return stub_usbaudio_has_volume_control; }

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                        \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                     \
	} while (0)

// 20 * log10 of what the scaling did, in hundredths of a dB, measured against
// the reference. Doubles in, because the deep end of the scale is measured on
// an average of dithered samples.
static double measured_db100(double scaled, double reference) {
	if (scaled <= 0) {
		return -9999999.0;
	}
	return 2000.0 * log10(scaled / reference);
}

static void test_usb_gain_is_the_decibels_asked_for(void) {
	// A handful of levels down the scale: a block of full-scale samples must
	// come out, on average, within a fraction of a decibel of what was asked
	// for. Averaged and not taken one sample at a time because the dither is
	// worth a couple of LSBs and the bottom of the scale is worth one.
	static const long DB100[] = {-1, -300, -600, -1200, -3000, -6000, -9000};
	for (unsigned i = 0; i < sizeof(DB100) / sizeof(DB100[0]); i++) {
		swvolume_set_usb_gain(DB100[i], false);
		enum { N = 1024 };
		static short buf[N];
		for (int j = 0; j < N; j++) {
			buf[j] = 32767;
		}
		swvolume_apply_s16(buf, N);
		double mean = 0;
		for (int j = 0; j < N; j++) {
			mean += (double)buf[j];
		}
		mean /= N;
		double got = measured_db100(mean, 32767.0);
		if (fabs(got - (double)DB100[i]) >= 30.0) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: asked %ld, measured %.1f (mean %.3f)\n", __FILE__, __LINE__, DB100[i], got,
					mean);
		}
	}

	// Positive is not a thing this route has: the gain is clamped at unity and
	// the samples pass through untouched.
	swvolume_set_usb_gain(600, false);
	short s = 12345;
	swvolume_apply_s16(&s, 1);
	CHECK(s == 12345);
}

static void test_mute_and_index_zero_are_silence(void) {
	short s[4] = {32767, -32768, 100, -100};
	swvolume_set_usb_gain(-600, true); // mute
	swvolume_apply_s16(s, 4);
	for (int i = 0; i < 4; i++) {
		CHECK(s[i] == 0);
	}

	// Index zero on the stock curve is an explicit mute as well.
	swvolume_clear_usb_gain();
	swvolume_set_index(0);
	short t[2] = {1234, -1234};
	swvolume_apply_s16(t, 2);
	CHECK(t[0] == 0 && t[1] == 0);
}

static void test_deep_attenuation_quiet_and_in_range(void) {
	// The bottom of the scale is where truncation used to leave grit and
	// whistling. What must come out instead is a small, clean tone: quiet,
	// entirely inside the range, with no wrapped or zeroed mess.
	swvolume_set_usb_gain(-6000, false); // -60 dB
	enum { N = 512 };
	static short buf[N];
	for (int i = 0; i < N; i++) {
		buf[i] = (short)(20000.0 * sin(2.0 * 3.14159265358979 * 10.0 * i / N));
	}
	swvolume_apply_s16(buf, N);

	int peak = 0;
	for (int i = 0; i < N; i++) {
		if (abs((int)buf[i]) > peak) {
			peak = abs((int)buf[i]);
		}
		// Nothing wrapped: the attenuated buffer is nowhere near the rails.
		CHECK(buf[i] > -200 && buf[i] < 200);
	}
	// 20000 * 10^(-60/20) = 20; the dither is worth a couple of LSBs.
	CHECK(peak > 10 && peak < 40);
}

static void test_full_scale_never_wraps(void) {
	// Near unity the output is the input scaled down by the gain's own
	// fraction -- but it must never wrap through the end of the range. A
	// sample at one rail must come back near the same rail, never flipped in
	// sign by a dithered step past the end.
	swvolume_set_usb_gain(-1, false); // the closest the gain gets to unity
	enum { N = 4096 };
	static short buf[N];
	for (int i = 0; i < N; i++) {
		buf[i] = (i & 1) ? 32767 : -32768;
	}
	swvolume_apply_s16(buf, N);
	for (int i = 0; i < N; i++) {
		if (i & 1) {
			CHECK(buf[i] > 32000); // still up at the top rail
		} else {
			CHECK(buf[i] < -32000); // and down at the bottom one
		}
	}
}

static void test_s32_rounds_and_keeps_depth(void) {
	// -6 dB is a gain of about 0.5012. A sample of 1 scaled by it is 0.5012
	// of an LSB: truncation would call that zero and bury the quietest codes
	// of a 32-bit signal in silence; rounding must call it one.
	swvolume_set_usb_gain(-600, false);
	int32_t s = 1;
	swvolume_apply_s32(&s, 1);
	CHECK(s == 1);

	// And 3 * 0.5012 = 1.5036 is a two, not the one flooring would leave.
	int32_t t = 3;
	swvolume_apply_s32(&t, 1);
	CHECK(t == 2);
}

static void test_s24_3le_stays_inside_the_three_bytes(void) {
	// 24-bit packed samples: a scaled answer must never carry a bit above
	// the 24th, or it lands in the next sample's bytes.
	swvolume_set_usb_gain(-300, false); // -3 dB
	unsigned char buf[3] = {0xFF, 0xFF, 0x7F}; // 0x7FFFFF, little-endian
	swvolume_apply_s24_3le(buf, 1);
	int32_t v = (int32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16));
	if (v & 0x800000) {
		v -= 0x1000000;
	}
	// 0x7FFFFF * 10^(-3/20) = 5938679 (0x5A9DF7); rounding is its one LSB.
	CHECK(v > 0x5A9DF7 - 2 && v < 0x5A9DF7 + 2);
}

static void test_active_only_on_the_usb_route(void) {
	// Nowhere but a device on the USB-C port: on Bluetooth and on the jacks
	// the samples are somebody else's business.
	stub_bluetooth_active = true;
	swvolume_set_usb_gain(-600, false);
	CHECK(!swvolume_active());
	short s = 1000;
	swvolume_apply_s16(&s, 1);
	CHECK(s == 1000);
	stub_bluetooth_active = false;

	stub_usbaudio_active = false;
	CHECK(!swvolume_active());
	swvolume_apply_s16(&s, 1);
	CHECK(s == 1000);
	stub_usbaudio_active = true;

	CHECK(swvolume_active()); // -6 dB on the port: the gain is in force
	swvolume_set_usb_gain(0, false);
	CHECK(!swvolume_active()); // unity: nothing to apply
}

static void test_index_curve_matches_its_own_coefficients(void) {
	// The stock curve's fallback (a card in play before usbaudio.c has set a
	// gain, on a device with no control of its own): the coefficient for an
	// index and what the scaling does with it are the same number, give or
	// the dither's couple of LSBs.
	swvolume_clear_usb_gain();
	for (int idx = 20; idx <= 90; idx += 10) {
		swvolume_set_index(idx);
		int64_t q31 = swvolume_coefficient(idx);
		short s = 16384;
		swvolume_apply_s16(&s, 1);
		int32_t want = (int32_t)(((int64_t)16384 * q31) >> 31);
		CHECK(abs((int)s - want) <= 2);
	}
}

int main(void) {
	test_usb_gain_is_the_decibels_asked_for();
	test_mute_and_index_zero_are_silence();
	test_deep_attenuation_quiet_and_in_range();
	test_full_scale_never_wraps();
	test_s32_rounds_and_keeps_depth();
	test_s24_3le_stays_inside_the_three_bytes();
	test_active_only_on_the_usb_route();
	test_index_curve_matches_its_own_coefficients();

	if (failures) {
		fprintf(stderr, "test_swvolume: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_swvolume: all checks passed\n");
	return 0;
}
