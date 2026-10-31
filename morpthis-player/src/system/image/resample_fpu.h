#ifndef RESAMPLE_FPU_H
#define RESAMPLE_FPU_H

#include <stdbool.h>
#include <stdint.h>

// Resampling written for the FPU rather than around it.
//
// This device is hard-float: single precision is a register move and one
// multiply, while the same work done in fixed point needs a shift per product
// and a wide accumulator to keep from clipping. Both routines below are
// separable -- the weights depend on x once and on y once, not on the pair --
// which is where most of the saving is, and both keep the inner loop free of
// branches, divisions and libm calls.
//
// The one place the interface layer can use this without turning every frame
// into float work is preparation: scaling an icon once when a page is built
// costs a few hundred microseconds, and removes a per-pixel transform from
// every frame that page is drawn in.

// Bilinear RGB888 magnification. `src` is tightly packed RGB888, `dst` is
// tightly packed RGB888. The crop is in source pixels. `flip_v` writes the
// output rows in reverse, which is what the now-playing backdrop wants.
bool resample_fpu_magnify(const uint8_t *src, int src_w, int src_h, int crop_x, int crop_y, int crop_w, int crop_h,
						  uint8_t *dst, int dst_w, int dst_h, bool flip_v);

// Separable tent filter over one 8-bit plane, for scaling an A8 mask to any
// size. Magnifying is bilinear; minifying widens the filter support with the
// ratio, so a 64 px glyph taken to 44 px averages what it covers instead of
// picking nearest neighbours out of it. `src_stride`/`dst_stride` are in bytes
// and may include padding.
bool resample_fpu_plane8(const uint8_t *src, int src_w, int src_h, int src_stride, uint8_t *dst, int dst_w, int dst_h,
						 int dst_stride);

// The same for ARGB8888 (B, G, R, A bytes in memory, as LVGL stores it). The
// colour channels are filtered in premultiplied space so a transparent edge
// does not pull its neighbours towards black, and are un-premultiplied again
// on the way out.
bool resample_fpu_argb8888(const uint8_t *src, int src_w, int src_h, int src_stride, uint8_t *dst, int dst_w,
						   int dst_h, int dst_stride);

#endif /* RESAMPLE_FPU_H */
