#include "fputest.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FPU_STRINGIFY_(x) #x
#define FPU_STRINGIFY(x) FPU_STRINGIFY_(x)

#if defined(__GNUC__) && __GNUC__ >= 8
#define FPU_NOINLINE __attribute__((noinline, noipa))
#elif defined(__GNUC__)
#define FPU_NOINLINE __attribute__((noinline))
#else
#define FPU_NOINLINE
#endif

// The compiler knows every input below is a constant. These helpers make it
// load them at run time instead: with -O3 the arithmetic would otherwise be
// folded at build time, and the test would never reach the FPU.
static FPU_NOINLINE float hold_f32(float v) {
	volatile float slot = v;
	return slot;
}

static FPU_NOINLINE double hold_f64(double v) {
	volatile double slot = v;
	return slot;
}

static FPU_NOINLINE int hold_i32(int v) {
	volatile int slot = v;
	return slot;
}

// GCC defines __mips_hard_float or __mips_soft_float from the ABI selected for
// the translation unit, and __mips_fpr with the width of a floating-point
// register. Reading those is how the page can say "hard-float FPR32" without a
// disassembler.
const char *fputest_abi(void) {
#if defined(__mips__) && defined(__mips_hard_float)
#if defined(__mips_fpr)
	return "hard-float FPR" FPU_STRINGIFY(__mips_fpr);
#else
	return "hard-float";
#endif
#elif defined(__mips__) && defined(__mips_soft_float)
	return "soft-float";
#elif defined(HOST_BUILD)
	return "host build";
#else
	return "unknown ABI";
#endif
}

typedef struct {
	int checks;
	int failed;
	char first_failure[96];
} fpu_test_result_t;

static uint32_t bits_f32(float v) {
	uint32_t bits;
	memcpy(&bits, &v, sizeof bits);
	return bits;
}

static uint64_t bits_f64(double v) {
	uint64_t bits;
	memcpy(&bits, &v, sizeof bits);
	return bits;
}

static void check_f32(fpu_test_result_t *r, const char *name, float got, uint32_t want) {
	uint32_t have = bits_f32(got);
	r->checks++;
	if (have != want) {
		if (r->failed == 0) {
			snprintf(r->first_failure, sizeof r->first_failure, "%s: got 0x%08lx, want 0x%08lx", name,
					 (unsigned long)have, (unsigned long)want);
		}
		r->failed++;
	}
}

static void check_f64(fpu_test_result_t *r, const char *name, double got, uint64_t want) {
	uint64_t have = bits_f64(got);
	r->checks++;
	if (have != want) {
		if (r->failed == 0) {
			snprintf(r->first_failure, sizeof r->first_failure, "%s: got 0x%016llx, want 0x%016llx", name,
					 (unsigned long long)have, (unsigned long long)want);
		}
		r->failed++;
	}
}

static void check_true(fpu_test_result_t *r, const char *name, bool ok) {
	r->checks++;
	if (!ok) {
		if (r->failed == 0) {
			snprintf(r->first_failure, sizeof r->first_failure, "%s", name);
		}
		r->failed++;
	}
}

bool fputest_run(char *out, size_t out_size) {
	fpu_test_result_t r = {0};

	float f_a = hold_f32(1.5f);
	float f_b = hold_f32(-2.25f);
	float f_one = hold_f32(1.0f);
	float f_half_ulp = hold_f32(0x1p-24f); // half a float ULP at 1.0
	float f_neg = hold_f32(-3.75f);
	int f_big = hold_i32(16777217); // 2^24 + 1, not representable as a float

	check_f32(&r, "float add", f_a + f_b, 0xBF400000u);				  // -0.75
	check_f32(&r, "float sub", f_a - f_b, 0x40700000u);				  // 3.75
	check_f32(&r, "float mul", f_a * f_b, 0xC0580000u);				  // -3.375
	check_f32(&r, "float div", f_b / f_a, 0xBFC00000u);				  // -1.5
	check_f32(&r, "float rounding", f_one + f_half_ulp, 0x3F800000u); // ties to even: stays 1.0
	check_true(&r, "float to int", (int)f_neg == -3);
	check_f32(&r, "int to float", (float)f_big, 0x4B800000u); // 2^24, ties to even

	// Special values are built from their bit patterns rather than by dividing
	// zero by zero, so the test does not depend on the FP exceptions mask.
	float nan_v;
	float inf_v;
	uint32_t nan_bits = 0x7FC00000u; // quiet NaN
	uint32_t inf_bits = 0x7F800000u; // +Inf
	memcpy(&nan_v, &nan_bits, sizeof nan_v);
	memcpy(&inf_v, &inf_bits, sizeof inf_v);
	nan_v = hold_f32(nan_v);
	inf_v = hold_f32(inf_v);

	float nan_sum = nan_v + f_one;
	float inf_mul = inf_v * f_a;
	check_true(&r, "NaN propagates", isnan(nan_sum) != 0);
	check_true(&r, "+Inf stays infinite", isinf(inf_mul) != 0 && inf_mul > 0.0f);

	float neg_zero = hold_f32(-0.0f);
	check_f32(&r, "negative zero", neg_zero * f_one, 0x80000000u);

	double d_a = hold_f64(1.5);
	double d_b = hold_f64(-2.25);
	double d_one = hold_f64(1.0);
	double d_half_ulp = hold_f64(0x1p-53); // half a double ULP at 1.0
	double d_neg = hold_f64(-3.75);
	double d_small = d_one + hold_f64(0x1p-40); // representable in double, not in float

	check_f64(&r, "double add", d_a + d_b, 0xBFE8000000000000ull);			  // -0.75
	check_f64(&r, "double mul", d_a * d_b, 0xC00B000000000000ull);			  // -3.375
	check_f64(&r, "double div", d_b / d_a, 0xBFF8000000000000ull);			  // -1.5
	check_f64(&r, "double rounding", d_one + d_half_ulp, 0x3FF0000000000000ull); // ties to even: stays 1.0
	check_true(&r, "double precision", d_small != 1.0 && (double)(float)d_small == 1.0);
	check_true(&r, "double to int", (int)d_neg == -3);

	check_true(&r, "IEEE 754 formats",
			   sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53);

	if (!out || out_size == 0) {
		return r.failed == 0;
	}

	if (r.failed == 0) {
		snprintf(out, out_size, "%s: %d/%d checks passed", fputest_abi(), r.checks, r.checks);
	} else {
		snprintf(out, out_size, "%s: %d/%d passed; first failure: %s", fputest_abi(), r.checks - r.failed,
				 r.checks, r.first_failure);
	}
	return r.failed == 0;
}
