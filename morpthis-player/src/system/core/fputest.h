#ifndef FPU_TEST_H
#define FPU_TEST_H

#include <stdbool.h>
#include <stddef.h>

// A small floating-point self-test for Developer options.
//
// It runs a dozen operations whose results are known bit for bit -- the four
// arithmetic operations in single and double precision, the tie-to-even
// rounding rule, conversions, NaN/Inf and signed zero -- and reports the first
// one that disagrees. It is not a benchmark and not a proof of IEEE 754; it is
// the quick answer to "is the arithmetic this binary performs sane on this
// machine?".
//
// The ABI is part of the answer. A build compiled with -msoft-float can pass
// every check through libgcc and never touch COP1, so fputest_run() names the
// ABI the compiler was told to use alongside the result.
const char *fputest_abi(void);

// Runs the checks. Writes a one-line result to `out` (when it is non-NULL and
// `out_size` is not zero) and returns true when all of them matched. Plain C,
// so it is equally valid in the host build.
bool fputest_run(char *out, size_t out_size);

#endif /* FPU_TEST_H */
