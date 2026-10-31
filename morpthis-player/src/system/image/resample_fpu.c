#include "resample_fpu.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Magnification: two source rows interpolated, reused across output rows
// ---------------------------------------------------------------------------

// One source row, interpolated horizontally into a float RGB row. The x table
// is shared by every row: x0/x1 are the two source columns, wx0/wx1 their
// weights, already summed to one.
static void scale_row(const uint8_t *src, int src_w, int crop_x, int sy, float *out, const int *x0, const int *x1,
					  const float *wx0, const float *wx1, int dst_w) {
	const uint8_t *base = src + (size_t)sy * (size_t)src_w * 3;
	for (int x = 0; x < dst_w; x++) {
		const uint8_t *p0 = base + ((size_t)crop_x + (size_t)x0[x]) * 3;
		const uint8_t *p1 = base + ((size_t)crop_x + (size_t)x1[x]) * 3;
		float a = wx0[x];
		float b = wx1[x];
		out[3 * x + 0] = (float)p0[0] * a + (float)p1[0] * b;
		out[3 * x + 1] = (float)p0[1] * a + (float)p1[1] * b;
		out[3 * x + 2] = (float)p0[2] * a + (float)p1[2] * b;
	}
}

bool resample_fpu_magnify(const uint8_t *src, int src_w, int src_h, int crop_x, int crop_y, int crop_w, int crop_h,
						  uint8_t *dst, int dst_w, int dst_h, bool flip_v) {
	if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || crop_w <= 0 || crop_h <= 0) {
		return false;
	}
	if (crop_x < 0 || crop_y < 0 || crop_x + crop_w > src_w || crop_y + crop_h > src_h) {
		return false;
	}

	// One allocation for the x table and the two interpolated rows: six small
	// malloc()s per cover add up when a track change asks for a cover and a
	// backdrop, and the tables are short-lived anyway.
	size_t x_count = (size_t)dst_w;
	size_t row_count = x_count * 3;
	size_t bytes = x_count * (2 * sizeof(int) + 2 * sizeof(float)) + row_count * 2 * sizeof(float);
	uint8_t *mem = malloc(bytes);
	if (!mem) {
		return false;
	}
	int *x0 = (int *)mem;
	int *x1 = x0 + x_count;
	float *wx0 = (float *)(x1 + x_count);
	float *wx1 = wx0 + x_count;
	float *row0 = (float *)(wx1 + x_count);
	float *row1 = row0 + row_count;

	// The horizontal weights, computed once. The 0.5 offsets sample at the
	// centre of the output pixel, which is what keeps an upscale symmetric.
	float xscale = (float)crop_w / (float)dst_w;
	for (int x = 0; x < dst_w; x++) {
		float fx = ((float)x + 0.5f) * xscale - 0.5f;
		if (fx < 0.0f) {
			fx = 0.0f;
		}
		int ix = (int)fx;
		if (ix >= crop_w) {
			ix = crop_w - 1;
		}
		float tx = fx - (float)ix;
		x0[x] = ix;
		x1[x] = ix + 1 < crop_w ? ix + 1 : ix;
		wx0[x] = 1.0f - tx;
		wx1[x] = tx;
	}

	float *rows[2] = {row0, row1};
	int row_y[2] = {-1, -1};
	float yscale = (float)crop_h / (float)dst_h;

	for (int y = 0; y < dst_h; y++) {
		float fy = ((float)y + 0.5f) * yscale - 0.5f;
		if (fy < 0.0f) {
			fy = 0.0f;
		}
		int iy = (int)fy;
		if (iy >= crop_h) {
			iy = crop_h - 1;
		}
		float ty = fy - (float)iy;
		int y0 = iy;
		int y1 = iy + 1 < crop_h ? iy + 1 : iy;

		// Keep the two rows that bracket this output row in the two slots. A
		// source row covers several output rows when magnifying, so the
		// horizontal pass is paid once per source row, not once per output row.
		int s0 = -1;
		for (int s = 0; s < 2; s++) {
			if (row_y[s] == y0) {
				s0 = s;
				break;
			}
		}
		if (s0 < 0) {
			int s;
			if (row_y[0] < 0) {
				s = 0;
			} else if (row_y[1] < 0) {
				s = 1;
			} else if (row_y[1] == y1) {
				s = 0; // the other slot is needed for y1
			} else {
				s = 1;
			}
			scale_row(src, src_w, crop_x, crop_y + y0, rows[s], x0, x1, wx0, wx1, dst_w);
			row_y[s] = y0;
			s0 = s;
		}

		int s1;
		if (y1 == y0) {
			s1 = s0;
		} else {
			s1 = -1;
			for (int s = 0; s < 2; s++) {
				if (row_y[s] == y1) {
					s1 = s;
					break;
				}
			}
			if (s1 < 0) {
				s1 = 1 - s0;
				scale_row(src, src_w, crop_x, crop_y + y1, rows[s1], x0, x1, wx0, wx1, dst_w);
				row_y[s1] = y1;
			}
		}

		const float *a = rows[s0];
		const float *b = rows[s1];
		float wy0 = 1.0f - ty;
		float wy1 = ty;
		int out_row = flip_v ? (dst_h - 1 - y) : y;
		uint8_t *out = dst + (size_t)out_row * (size_t)dst_w * 3;
		size_t channels = (size_t)dst_w * 3;
		for (size_t i = 0; i < channels; i++) {
			int v = (int)(a[i] * wy0 + b[i] * wy1);
			if (v < 0) {
				v = 0;
			} else if (v > 255) {
				v = 255;
			}
			out[i] = (uint8_t)v;
		}
	}

	free(mem);
	return true;
}

// ---------------------------------------------------------------------------
// General scaling: one normalized weight table per axis, two passes
// ---------------------------------------------------------------------------
//
// The axis table holds, for every output pixel, the first source pixel it
// reads, how many taps have a non-zero weight, and those weights. Taps that
// fall outside the filter's support are dropped rather than stored as zero, so
// the inner loop has no test in it and the common two-tap upscale runs twice.
typedef struct {
	int dst_n;
	int max_taps;
	int *first;
	int *count;
	float *weights; // dst_n * max_taps, rows normalized to one
} resample_axis_t;

static void axis_free(resample_axis_t *axis) {
	free(axis->first);
	free(axis->count);
	free(axis->weights);
	axis->first = NULL;
	axis->count = NULL;
	axis->weights = NULL;
}

// `support` is the half width of the tent in source pixels: one for an
// upscale, the ratio itself for a downscale. Widening it is what makes a
// minification average the pixels it passes over instead of aliasing them.
static bool axis_init(resample_axis_t *axis, int src_n, int dst_n) {
	memset(axis, 0, sizeof(*axis));
	axis->dst_n = dst_n;

	float scale = (float)src_n / (float)dst_n;
	float support = scale > 1.0f ? scale : 1.0f;
	int taps = (int)(2.0f * support) + 3;
	axis->max_taps = taps;
	axis->first = malloc((size_t)dst_n * sizeof(*axis->first));
	axis->count = malloc((size_t)dst_n * sizeof(*axis->count));
	axis->weights = malloc((size_t)dst_n * (size_t)taps * sizeof(*axis->weights));
	if (!axis->first || !axis->count || !axis->weights) {
		axis_free(axis);
		return false;
	}

	for (int i = 0; i < dst_n; i++) {
		float center = ((float)i + 0.5f) * scale;
		int lo = (int)(center - support) - 1;
		int hi = (int)(center + support) + 1;
		if (lo < 0) {
			lo = 0;
		}
		if (hi > src_n - 1) {
			hi = src_n - 1;
		}
		float *w = axis->weights + (size_t)i * (size_t)taps;
		float sum = 0.0f;
		for (int j = lo; j <= hi; j++) {
			float d = ((float)j + 0.5f) - center;
			if (d < 0.0f) {
				d = -d;
			}
			float v = d < support ? 1.0f - d / support : 0.0f;
			w[j - lo] = v;
			sum += v;
		}
		if (sum > 0.0f) {
			float inv = 1.0f / sum;
			for (int j = lo; j <= hi; j++) {
				w[j - lo] *= inv;
			}
		} else {
			w[0] = 1.0f;
		}

		// Drop the zero-weight taps at both ends: one at the lower edge comes
		// from the truncation of `center - support` into an int.
		int start = 0;
		int n = hi - lo + 1;
		while (n > 0 && w[start] == 0.0f) {
			start++;
			n--;
		}
		while (n > 0 && w[start + n - 1] == 0.0f) {
			n--;
		}
		if (n <= 0) {
			start = 0;
			n = 1;
			w[0] = 1.0f;
		} else if (start > 0) {
			memmove(w, w + start, (size_t)n * sizeof(*w));
		}
		axis->first[i] = lo + start;
		axis->count[i] = n;
	}
	return true;
}

static inline uint8_t clamp_u8(float v) {
	if (v <= 0.0f) {
		return 0;
	}
	if (v >= 255.0f) {
		return 255;
	}
	return (uint8_t)(v + 0.5f);
}

// A horizontal pass into an 8-bit intermediate, then a vertical pass out of
// it. The intermediate is `dst_w` wide and as tall as the source, so scaling
// a 128 px picture down to 75 px costs a 38 KB scratch buffer at most for the
// four-channel case, and nothing is kept afterwards.
static bool resample_two_pass(const uint8_t *src, int src_w, int src_h, int src_stride, uint8_t *dst, int dst_w,
							  int dst_h, int dst_stride, int channels, bool premultiplied_alpha) {
	resample_axis_t ax;
	resample_axis_t ay;
	if (!axis_init(&ax, src_w, dst_w)) {
		return false;
	}
	if (!axis_init(&ay, src_h, dst_h)) {
		axis_free(&ax);
		return false;
	}

	size_t tmp_stride = (size_t)dst_w * (size_t)channels;
	size_t tmp_bytes = tmp_stride * (size_t)src_h;
	uint8_t *tmp = NULL;
	if (tmp_bytes > 0 && tmp_bytes < (size_t)8 * 1024 * 1024) {
		tmp = malloc(tmp_bytes);
	}
	if (!tmp) {
		axis_free(&ax);
		axis_free(&ay);
		return false;
	}

	// Horizontal: every source row once, whatever the vertical ratio.
	for (int y = 0; y < src_h; y++) {
		const uint8_t *in = src + (size_t)y * (size_t)src_stride;
		uint8_t *out = tmp + (size_t)y * tmp_stride;
		for (int x = 0; x < dst_w; x++) {
			const float *w = ax.weights + (size_t)x * (size_t)ax.max_taps;
			const uint8_t *base = in + (size_t)ax.first[x] * (size_t)channels;
			int taps = ax.count[x];
			float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			for (int t = 0; t < taps; t++) {
				float wt = w[t];
				const uint8_t *p = base + (size_t)t * (size_t)channels;
				for (int c = 0; c < channels; c++) {
					acc[c] += (float)p[c] * wt;
				}
			}
			if (premultiplied_alpha && channels == 4) {
				float f = acc[3] * (1.0f / 255.0f);
				acc[0] *= f;
				acc[1] *= f;
				acc[2] *= f;
			}
			uint8_t *px = out + (size_t)x * (size_t)channels;
			for (int c = 0; c < channels; c++) {
				px[c] = clamp_u8(acc[c]);
			}
		}
	}

	// Vertical: taps rows of the intermediate per output row. One accumulator
	// per byte keeps the multiply-add chain in the FPU and the stores linear.
	for (int y = 0; y < dst_h; y++) {
		const float *w = ay.weights + (size_t)y * (size_t)ay.max_taps;
		const uint8_t *base = tmp + (size_t)ay.first[y] * tmp_stride;
		int taps = ay.count[y];
		uint8_t *out = dst + (size_t)y * (size_t)dst_stride;
		for (size_t col = 0; col < tmp_stride; col++) {
			float acc = 0.0f;
			for (int t = 0; t < taps; t++) {
				acc += (float)base[(size_t)t * tmp_stride + col] * w[t];
			}
			out[col] = clamp_u8(acc);
		}
		if (premultiplied_alpha && channels == 4) {
			for (int x = 0; x < dst_w; x++) {
				uint8_t *px = out + (size_t)x * 4;
				unsigned a = px[3];
				if (a == 0) {
					px[0] = 0;
					px[1] = 0;
					px[2] = 0;
					continue;
				}
				float inv = 255.0f / (float)a;
				px[0] = clamp_u8((float)px[0] * inv);
				px[1] = clamp_u8((float)px[1] * inv);
				px[2] = clamp_u8((float)px[2] * inv);
			}
		}
	}

	free(tmp);
	axis_free(&ax);
	axis_free(&ay);
	return true;
}

bool resample_fpu_plane8(const uint8_t *src, int src_w, int src_h, int src_stride, uint8_t *dst, int dst_w, int dst_h,
						 int dst_stride) {
	if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
		return false;
	}
	if (src_stride < src_w || dst_stride < dst_w) {
		return false;
	}
	return resample_two_pass(src, src_w, src_h, src_stride, dst, dst_w, dst_h, dst_stride, 1, false);
}

bool resample_fpu_argb8888(const uint8_t *src, int src_w, int src_h, int src_stride, uint8_t *dst, int dst_w,
						   int dst_h, int dst_stride) {
	if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
		return false;
	}
	if (src_stride < src_w * 4 || dst_stride < dst_w * 4) {
		return false;
	}

	// Opaque art needs no premultiplying, and skipping it keeps every channel
	// exactly where it was instead of a rounding step off it: most of the
	// coloured icons are solid pictures with an alpha of 255 throughout.
	bool opaque = true;
	for (int y = 0; y < src_h && opaque; y++) {
		const uint8_t *row = src + (size_t)y * (size_t)src_stride;
		for (int x = 0; x < src_w; x++) {
			if (row[(size_t)x * 4 + 3] != 255) {
				opaque = false;
				break;
			}
		}
	}

	return resample_two_pass(src, src_w, src_h, src_stride, dst, dst_w, dst_h, dst_stride, 4, !opaque);
}
