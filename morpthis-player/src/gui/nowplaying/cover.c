#include "cover.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"

#include "src/gui/board_profile.h"
#include "src/system/library/albumart.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/core/utils.h"

// The backdrop is blurred at half resolution and stretched back up. Blurring
// the small copy (rather than relying on the stretch alone) is what keeps it
// smooth: a plain upscale of a heavily shrunk image shows the interpolation
// facets as visible blocks.
#define BACKDROP_SCALE_DIVISOR 2

// Box blur radius, in pixels of the half-size image. Repeated passes of a box
// blur approximate a gaussian closely enough that nothing looks boxy.
#define BACKDROP_BLUR_RADIUS 9
#define BACKDROP_BLUR_PASSES 3

// A frosted band keeps more of the picture than a blur a title is read on: it
// is looked through, and what sells it as glass is the colour masses of the
// photograph still standing in it -- the glow, the corner of green -- where the
// stronger treatment above leaves a wash. So the band is blurred well under
// half the radius and one pass fewer, which on the V1's deck leaves masses of
// a dozen pixels or so, as in the look this follows.
#define BACKDROP_BAND_BLUR_RADIUS 4
#define BACKDROP_BAND_BLUR_PASSES 2

// The width the blur radii were chosen against: a 240-pixel backdrop, whose
// half-size copy (BACKDROP_SCALE_DIVISOR) is this across. The radii are scaled
// by it, so they stay a fraction of the picture when the box changes size.
#define BACKDROP_BLUR_REF_W 120

// How far the band pulls each channel away from the pixel's own grey, in per
// cent of that distance added on top: the tint that keeps a frosted sleeve a
// colour and not a wash, after the dim has taken its light (see make_backdrop).
// 100 would leave the hue as the dim left it.
#define BACKDROP_BAND_SAT_PCT 140

// How far the backdrop is pushed towards black (dark theme) or white (light
// theme) so the text on top of it stays readable. 100 = untouched.
#define BACKDROP_BRIGHTNESS_PCT 42

static bool backdrop_light;
static bool backdrop_upright;
static bool backdrop_from_bottom;
static int backdrop_band_pct;

void cover_set_backdrop_light(bool light) { backdrop_light = light; }

void cover_set_backdrop_upright(bool upright) { backdrop_upright = upright; }

void cover_set_backdrop_from_bottom(bool from_bottom) { backdrop_from_bottom = from_bottom; }

void cover_set_backdrop_band_brightness(int pct) { backdrop_band_pct = pct; }

// ---------------------------------------------------------------------------
// raw (decoded, RGB888) images
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *pixels; // RGB888, w*h*3 bytes
	int w;
	int h;
} raw_image_t;

// A rectangle of a source image, in source pixels.
typedef struct {
	int x;
	int y;
	int w;
	int h;
} crop_t;

static inline uint16_t rgb_to_rgb565(uint32_t r, uint32_t g, uint32_t b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The same, rounded to the nearest 5- and 6-bit level instead of cut down to
// the one below: round(v * 31 / 255) and round(v * 63 / 255), exact for every
// input byte. Half the worst-case error of the cut, which on a flat grey is
// the difference between grey and a faint tint.
static inline uint16_t rgb_to_rgb565_rounded(uint32_t r, uint32_t g, uint32_t b) {
	uint32_t r5 = (r * 249 + 1014) >> 11;
	uint32_t g6 = (g * 253 + 505) >> 10;
	uint32_t b5 = (b * 249 + 1014) >> 11;
	return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void raw_free(raw_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	img->pixels = NULL;
}

// How much memory a single decode may take.
//
// On the device the budget is small and re-read every time: a quarter of what is
// free now, capped at 8 MB. It must not be derived from MemAvailable, which is
// mostly page cache -- mapped fonts, the player's own code, the audio decoder's
// read-ahead. The kernel cannot kill the player (it runs at oom_score_adj -500),
// so a large allocation is served by evicting all of that cache, and every glyph
// draw and instruction fetch then becomes a page fault against slow flash.
//
// A refused cover is a glyph; a thrashing kernel is a frozen device. The cap is
// what keeps the second from happening, and a larger one buys no artwork: the
// JPEG path decodes at half, a quarter or an eighth (see jpeg_planes.h), so even
// a poster-sized progressive cover fits in two megabytes. The host build keeps a
// large budget so a PC music library loses no artwork.
#define DECODE_BUDGET_MIN_BYTES (2 * 1024 * 1024)
#ifdef HOST_BUILD
#define DECODE_BUDGET_MAX_BYTES (64 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 1
#define DECODE_BUDGET_RESERVE_BYTES (3 * 1024 * 1024)
#else
#define DECODE_BUDGET_MAX_BYTES (8 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 4
#define DECODE_BUDGET_RESERVE_BYTES 0
#endif

// Set only while the album artwork pass runs (see cover_precache_albums). One
// image at a time, with the scan's progress page up and nothing else asking
// for pixels.
static volatile bool decode_budget_relaxed;

static size_t read_mem_available(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return 0;
	}

	char line[256];
	size_t kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1) {
			break;
		}
	}

	fclose(f);
	return kb * 1024;
}

static size_t decode_budget_bytes(void) {
	// Recomputed on every call: what was free at boot says nothing about what is
	// free after an hour of playing. /proc/meminfo costs a few microseconds and
	// this only runs on the worker thread.
	size_t available = read_mem_available();
	size_t divisor = DECODE_BUDGET_DIVISOR;
	// The artwork pass runs with the progress page up and nothing else asking
	// for pixels, so it may take a larger share. It is also the one caller that
	// cannot retry later: a cover refused here stays uncached until somebody
	// opens the album, where it is refused again for the same reason.
	if (decode_budget_relaxed && divisor > 1) {
		divisor = 2;
	}
	size_t budget = available / divisor;

	if (budget > DECODE_BUDGET_RESERVE_BYTES) {
		budget -= DECODE_BUDGET_RESERVE_BYTES;
	}

	if (budget < DECODE_BUDGET_MIN_BYTES)
		budget = DECODE_BUDGET_MIN_BYTES;
	if (budget > DECODE_BUDGET_MAX_BYTES)
		budget = DECODE_BUDGET_MAX_BYTES;

	return budget;
}

// Peak bytes per source pixel while stb_image decodes a PNG: four channels out,
// plus the filtered scanline buffer. JPEGs need no estimate: the plane decoder
// works their peak out from the frame header and refuses on its own.
#define PNG_DECODE_BYTES_PER_PIXEL 8

// Milliseconds from an arbitrary origin, for the decode log. Only ever
// subtracted from itself, so the origin does not matter.
static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// Decodes compressed JPEG/PNG bytes to RGB888. Alpha is dropped rather than
// composited: covers are opaque in practice, and three channels keep the
// intermediate buffer smaller.
//
// max_box is the largest output anybody wants from this decode. Baseline JPEGs,
// which is what album art overwhelmingly is, go through the vendored TJpgDec
// with its 1/2, 1/4, 1/8 output scaling: a 3000x3000 cover comes out as 750x750
// in under two megabytes instead of a 45 MB full-resolution pass this device
// cannot afford. That is how the stock player shows poster-sized covers on the
// same 64 MB. Progressive JPEGs and PNGs fall back to stb_image under the
// memory budget.
static bool decode_raw(const uint8_t *data, size_t size, int max_box, raw_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!data || size == 0)
		return false;

	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8 && max_box > 0) {
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint64_t t0 = now_ms();
		uint8_t *pixels = jpeg_scaled_decode(data, size, max_box, &w, &h, &src_w, &src_h);
		if (pixels) {
			// Both sizes, because the output on its own says nothing about what
			// happened: TJpgDec only ever halves, and a picture already smaller
			// than the box comes out untouched, so one number alone would make a
			// 360x360 cover look like something that had been reduced. The time
			// is what says whether a slow cover is a big one or a stuck one.
			fprintf(stderr, "cover: decoded jpeg %dx%d -> %dx%d in %llu ms\n", src_w, src_h, w, h,
					(unsigned long long)(now_ms() - t0));
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}

		// Not readable by TJpgDec, which in practice means progressive. Those go
		// through stb, but not through its ordinary loader: that one interleaves
		// a full-resolution RGB buffer next to the planes it was built from,
		// which for a 1200x1200 cover peaks around 11 MB -- refused by any sane
		// budget on a 64 MB device.
		//
		// The plane decoder averages the components down to the size actually
		// wanted, and, when even that does not fit, decodes at half, a quarter or
		// an eighth by keeping only the low-frequency corner of each block. It
		// picks the best scale the budget allows and says which in the log. The
		// 2x margin is the same one the TJpgDec path leaves for the resampler
		// below.
		size_t peak = 0;
		uint64_t t1 = now_ms();
		pixels = jpeg_planes_decode(data, size, max_box * 2, decode_budget_bytes(), &w, &h, &peak);
		if (pixels) {
			fprintf(stderr, "cover: decoded progressive jpeg to %dx%d (planes, peak about %zu KB) in %llu ms\n", w, h,
					peak / 1024, (unsigned long long)(now_ms() - t1));
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		if (peak > 0) {
			fprintf(stderr, "cover: skipping jpeg artwork, decoding it would need about %zu KB (budget %zu KB)\n",
					peak / 1024, decode_budget_bytes() / 1024);
		}
		return false;
	}

	if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && max_box > 0) {
		// The streaming decoder, not stb: stb inflates the whole image first,
		// which for a 3000x3000 file is far more than a 64 MB device can spare.
		// This one works scanline by scanline and downsamples as it goes, so the
		// source size stops mattering. The 2x margin gives the final resampler
		// enough to average over, as on the JPEG path.
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint64_t t0 = now_ms();
		uint8_t *pixels = png_scaled_decode(data, size, max_box * 2, &w, &h, &src_w, &src_h);
		if (pixels) {
			fprintf(stderr, "cover: decoded png %dx%d -> %dx%d in %llu ms\n", src_w, src_h, w, h,
					(unsigned long long)(now_ms() - t0));
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		// Interlaced or 16-bit: falls through to stb, under the memory budget.
	}

	int w = 0, h = 0, comp = 0;
	if (!stbi_info_from_memory(data, (int)size, &w, &h, &comp))
		return false;
	if (w <= 0 || h <= 0)
		return false;
	int bpp = PNG_DECODE_BYTES_PER_PIXEL;
	size_t needed = (size_t)w * (size_t)h * (size_t)bpp;
	size_t budget = decode_budget_bytes();
	if (needed > budget) {
		// Better no artwork than an allocation that wedges the whole player
		// halfway down a folder.
		fprintf(stderr, "cover: skipping %dx%d artwork, decoding it would need about %zu KB (budget %zu KB)\n", w, h,
				needed / 1024, budget / 1024);
		return false;
	}

	uint8_t *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 3);
	if (!pixels) {
		fprintf(stderr, "cover: decode of %dx%d artwork failed\n", w, h);
		return false;
	}

	// One line per decode: this is the most expensive thing the browser does, and
	// the log shows how many decodes a folder actually triggers.
	fprintf(stderr, "cover: decoded %dx%d (%d bytes/px)\n", w, h, bpp);

	out->pixels = pixels;
	out->w = w;
	out->h = h;
	return true;
}

// The largest centred rectangle of the source with the target's aspect ratio.
// This turns a square cover into an edge-to-edge banner: the sides stay put and
// the overflow at top and bottom is simply never sampled.
static crop_t crop_to_aspect(int src_w, int src_h, int dst_w, int dst_h) {
	crop_t c;

	if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
		c.h = src_h;
		c.w = (int)((int64_t)src_h * dst_w / dst_h);
	} else {
		c.w = src_w;
		c.h = (int)((int64_t)src_w * dst_h / dst_w);
	}
	if (c.w < 1)
		c.w = 1;
	if (c.h < 1)
		c.h = 1;

	c.x = (src_w - c.w) / 2;
	c.y = (src_h - c.h) / 2;
	return c;
}

// ---------------------------------------------------------------------------
// Shrinking by area
//
// Each output pixel is the average of the source area it covers, with the
// pixels cut by its edges counted for the part that falls inside. The plain box
// average below counts whole pixels only, so at a ratio that is not a whole
// number its footprint alternates between one and two source pixels: 600 to 480
// takes one column in four twice. Thin lines, circles and lettering then come
// out uneven, a stroke thicker in one place and thinner a few pixels on.
//
// Weights are in 1/AREA_ONE of an output pixel along each axis, and add up to
// exactly AREA_ONE, so a flat colour stays that colour. Two axes of 12 bits and
// 8-bit samples fill 32 bits and no more: 4096 * 4096 * 255 plus the rounding
// half is below 2^32.
// ---------------------------------------------------------------------------

#define AREA_SHIFT 12
#define AREA_ONE (1 << AREA_SHIFT)

// The source pixels under one output pixel: `count` of them from `first`, with
// their weights from `weight_at` in the axis' weight table.
typedef struct {
	int first;
	int count;
	int weight_at;
} area_span_t;

// Fills the spans and weights of one axis, src_len source pixels onto dst_len
// output pixels, dst_len <= src_len. `weights` holds src_len + dst_len entries,
// which is enough: every source pixel belongs to one output pixel, except the
// ones an edge cuts, which belong to two. False when a ratio is so large that the
// weights cannot be made to add up; the caller then shrinks the old way.
static bool area_axis(int src_len, int dst_len, area_span_t *spans, uint16_t *weights) {
	int at = 0;
	for (int d = 0; d < dst_len; d++) {
		// Positions in 1/dst_len of a source pixel: output pixel d covers
		// [lo, hi), source pixel s covers [s * dst_len, (s + 1) * dst_len).
		int64_t lo = (int64_t)d * src_len;
		int64_t hi = lo + src_len;
		int first = (int)(lo / dst_len);
		int last = (int)((hi - 1) / dst_len);
		if (last >= src_len)
			last = src_len - 1;

		spans[d].first = first;
		spans[d].count = last - first + 1;
		spans[d].weight_at = at;

		int sum = 0, biggest = at;
		for (int s = first; s <= last; s++) {
			int64_t s_lo = (int64_t)s * dst_len;
			int64_t s_hi = s_lo + dst_len;
			int64_t overlap = (hi < s_hi ? hi : s_hi) - (lo > s_lo ? lo : s_lo);
			int w = (int)((overlap * AREA_ONE + src_len / 2) / src_len);
			weights[at] = (uint16_t)w;
			if (w > weights[biggest])
				biggest = at;
			sum += w;
			at++;
		}

		// Rounding leaves the sum a little off AREA_ONE; the largest weight
		// takes the difference.
		int fixed = weights[biggest] + (AREA_ONE - sum);
		if (fixed < 0 || fixed > AREA_ONE)
			return false;
		weights[biggest] = (uint16_t)fixed;
	}
	return true;
}

// resample_rgb() for a shrink, by area. NULL when a table cannot be had.
static uint8_t *resample_area(const raw_image_t *src, const crop_t *crop, int dst_w, int dst_h, bool flip_v) {
	if (dst_w > crop->w || dst_h > crop->h)
		return NULL;

	size_t row_len = (size_t)dst_w * 3;
	area_span_t *xs = malloc((size_t)dst_w * sizeof(*xs));
	area_span_t *ys = malloc((size_t)dst_h * sizeof(*ys));
	uint16_t *xw = malloc((size_t)(crop->w + dst_w) * sizeof(*xw));
	uint16_t *yw = malloc((size_t)(crop->h + dst_h) * sizeof(*yw));
	uint32_t *acc = malloc(row_len * sizeof(*acc));
	uint32_t *line = malloc(row_len * sizeof(*line));
	uint8_t *dst = malloc((size_t)dst_h * row_len);

	bool ok = xs && ys && xw && yw && acc && line && dst && area_axis(crop->w, dst_w, xs, xw) &&
			  area_axis(crop->h, dst_h, ys, yw);
	if (ok) {
		int cached = -1; // the source row `line` holds, shrunk across

		for (int y = 0; y < dst_h; y++) {
			memset(acc, 0, row_len * sizeof(*acc));

			for (int k = 0; k < ys[y].count; k++) {
				int s = ys[y].first + k;
				uint32_t wy = yw[ys[y].weight_at + k];

				// One source row across: at most AREA_ONE * 255 per sample.
				// Consecutive output rows share the row an edge cuts, so the
				// last one is kept.
				if (s != cached) {
					const uint8_t *in = src->pixels + ((size_t)(crop->y + s) * src->w + crop->x) * 3;
					uint32_t *o = line;
					for (int x = 0; x < dst_w; x++) {
						const uint8_t *px = in + (size_t)xs[x].first * 3;
						const uint16_t *w = xw + xs[x].weight_at;
						uint32_t r = 0, g = 0, b = 0;
						for (int i = 0; i < xs[x].count; i++) {
							r += w[i] * (uint32_t)px[0];
							g += w[i] * (uint32_t)px[1];
							b += w[i] * (uint32_t)px[2];
							px += 3;
						}
						o[0] = r;
						o[1] = g;
						o[2] = b;
						o += 3;
					}
					cached = s;
				}

				for (size_t i = 0; i < row_len; i++)
					acc[i] += wy * line[i];
			}

			int out_row = flip_v ? (dst_h - 1 - y) : y;
			uint8_t *out = dst + (size_t)out_row * row_len;
			for (size_t i = 0; i < row_len; i++)
				out[i] = (uint8_t)((acc[i] + (1u << (2 * AREA_SHIFT - 1))) >> (2 * AREA_SHIFT));
		}
	}

	free(xs);
	free(ys);
	free(xw);
	free(yw);
	free(acc);
	free(line);
	if (!ok) {
		free(dst);
		return NULL;
	}
	return dst;
}

// Resamples a crop of the source into a dst_w x dst_h RGB888 buffer, optionally
// upside down. It averages over the source footprint when shrinking, which
// keeps thumbnails from turning into noise, and interpolates when growing, so
// small covers stretched edge to edge do not come out blocky.
static uint8_t *resample_rgb(const raw_image_t *src, const crop_t *crop, int dst_w, int dst_h, bool flip_v) {
	if (dst_w < 1 || dst_h < 1)
		return NULL;

	bool magnify = (dst_w >= crop->w);

	// A shrink goes by area (above). The loop below stays for the case where
	// that cannot get its tables, and for growing.
	if (!magnify) {
		uint8_t *by_area = resample_area(src, crop, dst_w, dst_h, flip_v);
		if (by_area)
			return by_area;
	}

	uint8_t *dst = malloc((size_t)dst_w * dst_h * 3);
	if (!dst)
		return NULL;

	for (int y = 0; y < dst_h; y++) {
		int out_row = flip_v ? (dst_h - 1 - y) : y;
		uint8_t *dst_row = dst + (size_t)out_row * dst_w * 3;

		for (int x = 0; x < dst_w; x++) {
			uint32_t r, g, b;

			if (magnify) {
				// Bilinear: sample the source at the centre of this output pixel.
				float fx = ((float)x + 0.5f) * crop->w / dst_w - 0.5f;
				float fy = ((float)y + 0.5f) * crop->h / dst_h - 0.5f;
				if (fx < 0)
					fx = 0;
				if (fy < 0)
					fy = 0;

				int x0 = (int)fx, y0 = (int)fy;
				int x1 = x0 + 1 < crop->w ? x0 + 1 : x0;
				int y1 = y0 + 1 < crop->h ? y0 + 1 : y0;
				float tx = fx - x0, ty = fy - y0;

				const uint8_t *p00 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x0) * 3;
				const uint8_t *p10 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x1) * 3;
				const uint8_t *p01 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x0) * 3;
				const uint8_t *p11 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x1) * 3;

				float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
				float w01 = (1 - tx) * ty, w11 = tx * ty;

				r = (uint32_t)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11);
				g = (uint32_t)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11);
				b = (uint32_t)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11);
			} else {
				// Box average over every source pixel this output pixel covers.
				int sy0 = crop->y + (int)((int64_t)y * crop->h / dst_h);
				int sy1 = crop->y + (int)((int64_t)(y + 1) * crop->h / dst_h);
				int sx0 = crop->x + (int)((int64_t)x * crop->w / dst_w);
				int sx1 = crop->x + (int)((int64_t)(x + 1) * crop->w / dst_w);
				if (sy1 <= sy0)
					sy1 = sy0 + 1;
				if (sx1 <= sx0)
					sx1 = sx0 + 1;

				uint32_t sr = 0, sg = 0, sb = 0, n = 0;
				for (int sy = sy0; sy < sy1; sy++) {
					const uint8_t *row = src->pixels + (size_t)sy * src->w * 3;
					for (int sx = sx0; sx < sx1; sx++) {
						const uint8_t *px = row + (size_t)sx * 3;
						sr += px[0];
						sg += px[1];
						sb += px[2];
						n++;
					}
				}
				r = sr / n;
				g = sg / n;
				b = sb / n;
			}

			uint8_t *out_px = dst_row + (size_t)x * 3;
			out_px[0] = (uint8_t)r;
			out_px[1] = (uint8_t)g;
			out_px[2] = (uint8_t)b;
		}
	}

	return dst;
}

// One separable box-blur pass over an RGB888 buffer, using a running sum so the
// cost does not grow with the radius. scratch must hold w*h*3 bytes.
static void box_blur_pass(uint8_t *img, uint8_t *scratch, int w, int h, int radius) {
	if (radius < 1)
		return;

	// horizontal: img -> scratch
	for (int y = 0; y < h; y++) {
		const uint8_t *row = img + (size_t)y * w * 3;
		uint8_t *out = scratch + (size_t)y * w * 3;

		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int x = 0; x <= radius && x < w; x++) {
			sum[0] += row[x * 3];
			sum[1] += row[x * 3 + 1];
			sum[2] += row[x * 3 + 2];
			count++;
		}

		for (int x = 0; x < w; x++) {
			out[x * 3] = (uint8_t)(sum[0] / count);
			out[x * 3 + 1] = (uint8_t)(sum[1] / count);
			out[x * 3 + 2] = (uint8_t)(sum[2] / count);

			int add = x + radius + 1;
			int drop = x - radius;
			if (add < w) {
				sum[0] += row[add * 3];
				sum[1] += row[add * 3 + 1];
				sum[2] += row[add * 3 + 2];
				count++;
			}
			if (drop >= 0) {
				sum[0] -= row[drop * 3];
				sum[1] -= row[drop * 3 + 1];
				sum[2] -= row[drop * 3 + 2];
				count--;
			}
		}
	}

	// vertical: scratch -> img
	for (int x = 0; x < w; x++) {
		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int y = 0; y <= radius && y < h; y++) {
			const uint8_t *px = scratch + ((size_t)y * w + x) * 3;
			sum[0] += px[0];
			sum[1] += px[1];
			sum[2] += px[2];
			count++;
		}

		for (int y = 0; y < h; y++) {
			uint8_t *out = img + ((size_t)y * w + x) * 3;
			out[0] = (uint8_t)(sum[0] / count);
			out[1] = (uint8_t)(sum[1] / count);
			out[2] = (uint8_t)(sum[2] / count);

			int add = y + radius + 1;
			int drop = y - radius;
			if (add < h) {
				const uint8_t *px = scratch + ((size_t)add * w + x) * 3;
				sum[0] += px[0];
				sum[1] += px[1];
				sum[2] += px[2];
				count++;
			}
			if (drop >= 0) {
				const uint8_t *px = scratch + ((size_t)drop * w + x) * 3;
				sum[0] -= px[0];
				sum[1] -= px[1];
				sum[2] -= px[2];
				count--;
			}
		}
	}
}

// Ordered-dither offsets, one per position of a 4x4 tile. Added before the 565
// truncation they spread the quantisation error around, turning the backdrop's
// banded gradients into a smooth ramp. Range is +-half of the 5-bit step.
static const int8_t BAYER4[4][4] = {
	{-4, 2, -3, 3},
	{4, -2, 5, -1},
	{-3, 3, -4, 2},
	{5, -1, 4, -2},
};

static inline uint8_t dither_channel(int value, int offset) {
	value += offset;
	if (value < 0)
		value = 0;
	if (value > 255)
		value = 255;
	return (uint8_t)value;
}

// pack_rgb565 with the ordered dither above. Used for the blurred backdrops,
// whose soft gradients are exactly where 16-bit banding shows.
static bool pack_rgb565_dithered(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *px = rgb + ((size_t)y * w + x) * 3;
			int offset = BAYER4[y & 3][x & 3];
			buf[(size_t)y * w + x] = rgb_to_rgb565(dither_channel(px[0], offset), dither_channel(px[1], offset),
												   dither_channel(px[2], offset));
		}
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// Packs an RGB888 buffer into the cover_image_t LVGL draws from. Takes
// ownership of nothing: the source buffer is still the caller's to free.
static bool pack_rgb565(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int i = 0; i < w * h; i++) {
		buf[i] = rgb_to_rgb565_rounded(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// The source rectangle and output size a fit asks for. COVER_FIT_COVER crops
// to the box's aspect ratio and fills it; anything else fits the whole image
// inside the box, never blowing it up.
static void scaled_geometry(const raw_image_t *src, int box_w, int box_h, cover_fit_t fit, crop_t *crop, int *dst_w,
							int *dst_h) {
	if (fit == COVER_FIT_COVER) {
		*crop = crop_to_aspect(src->w, src->h, box_w, box_h);
		*dst_w = box_w;
		*dst_h = box_h;
		return;
	}

	*crop = (crop_t){0, 0, src->w, src->h};
	*dst_w = src->w;
	*dst_h = src->h;

	if (*dst_w > box_w) {
		*dst_h = (int)((int64_t)*dst_h * box_w / *dst_w);
		*dst_w = box_w;
	}
	if (*dst_h > box_h) {
		*dst_w = (int)((int64_t)*dst_w * box_h / *dst_h);
		*dst_h = box_h;
	}
	if (*dst_w < 1)
		*dst_w = 1;
	if (*dst_h < 1)
		*dst_h = 1;
}

// Builds the visible picture: COVER_FIT_COVER crops to the box's aspect ratio,
// anything else fits the whole image inside the box.
static bool make_scaled(const raw_image_t *src, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop;
	int dst_w, dst_h;
	scaled_geometry(src, box_w, box_h, fit, &crop, &dst_w, &dst_h);

	uint8_t *rgb = resample_rgb(src, &crop, dst_w, dst_h, false);
	if (!rgb)
		return false;

	bool ok = pack_rgb565(rgb, dst_w, dst_h, out);
	free(rgb);
	return ok;
}

// Builds the upside-down, blurred, dimmed copy that sits behind the controls.
// The blur is a shrink, box-blur passes, then an interpolated stretch: visually
// a soft gaussian-like smear for a few hundred kilobytes of work.
//
// `over_w` x `over_h`, when given with from_bottom, is the box the picture this
// band stands over was itself made at: the band is then cut out of THAT crop --
// its bottom `box_h / over_h` -- rather than out of a crop of the band's own
// aspect. The two are not the same rectangle: a crop aimed at a wide short box
// takes in more of the source's sides than the crop aimed at the screen the
// band frosts, and a blur whose colours come from pixels that are not on the
// screen meets the photograph above it as a seam of the wrong colour.
static bool make_backdrop(const raw_image_t *src, int box_w, int box_h, bool from_bottom, int over_w, int over_h,
						  cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop = crop_to_aspect(src->w, src->h, box_w, box_h);
	if (from_bottom) {
		if (over_w > 0 && over_h > 0) {
			crop_t over = crop_to_aspect(src->w, src->h, over_w, over_h);
			int band_h = (int)((int64_t)over.h * box_h / over_h);
			if (band_h < 1)
				band_h = 1;
			if (band_h > over.h)
				band_h = over.h;
			crop.x = over.x;
			crop.w = over.w;
			crop.h = band_h;
			crop.y = over.y + over.h - band_h; // the band the block will be drawn over
		} else {
			crop.y = src->h - crop.h; // the band the block will be drawn over
			if (crop.y < 0)
				crop.y = 0;
		}
	}

	int small_w = box_w / BACKDROP_SCALE_DIVISOR;
	int small_h = box_h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2)
		small_w = 2;
	if (small_h < 2)
		small_h = 2;

	// A frosted band -- the foot of the picture, with the box it stands over --
	// is treated lighter than a blur a title is read on: blurred at a smaller
	// radius so the photograph's colour masses stay standing in the glass, and
	// dimmed and tinted by its own numbers below.
	bool band = from_bottom && over_h > 0;

	// Flip while shrinking, so the stretch back up only has to interpolate and
	// no separate flip pass is needed. Not for a bottom band: that one stands
	// in for the pixels underneath it and has to face the same way. Nor when
	// the backdrop is the whole screen with the sleeve shown over it, where the
	// two are plainly the same picture and one of them being upside down is the
	// only thing anybody sees.
	bool flip = !from_bottom && !backdrop_upright;
	uint8_t *small = resample_rgb(src, &crop, small_w, small_h, flip);
	if (!small)
		return false;

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		// A blur is a fraction of the picture, not a fixed number of pixels.
		// Keeping the radius absolute would double the softness as soon as the
		// box was halved -- which is exactly what asking for a half-screen
		// backdrop does.
		int radius = band ? BACKDROP_BAND_BLUR_RADIUS : BACKDROP_BLUR_RADIUS;
		radius = (radius * small_w + BACKDROP_BLUR_REF_W / 2) / BACKDROP_BLUR_REF_W;
		int limit = small_w / 2;
		if (limit < 1) {
			limit = 1;
		}
		if (radius < 1) {
			radius = 1;
		}
		if (radius > limit) {
			radius = limit;
		}

		int passes = band ? BACKDROP_BAND_BLUR_PASSES : BACKDROP_BLUR_PASSES;
		for (int i = 0; i < passes; i++) {
			box_blur_pass(small, scratch, small_w, small_h, radius);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t whole = {0, 0, small_w, small_h};

	uint8_t *rgb = resample_rgb(&tiny, &whole, box_w, box_h, false);
	free(small);
	if (!rgb)
		return false;

	// Push the backdrop away from the text colour: whatever the cover happens to
	// be, the title has to stay readable on top of it. A band that frosts the
	// picture rather than standing in for its reflection keeps more of its own
	// light -- it is looked through, not at -- so it takes its own percentage
	// when the caller set one (see cover_set_backdrop_band_brightness).
	int pct = BACKDROP_BRIGHTNESS_PCT;
	if (band && backdrop_band_pct > 0) {
		pct = backdrop_band_pct;
	}
	for (int i = 0; i < box_w * box_h * 3; i += 3) {
		int r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
		if (backdrop_light) {
			r = 255 - ((255 - r) * pct) / 100;
			g = 255 - ((255 - g) * pct) / 100;
			b = 255 - ((255 - b) * pct) / 100;
		} else {
			r = (r * pct) / 100;
			g = (g * pct) / 100;
			b = (b * pct) / 100;
		}
		if (band) {
			// The glass tint. Dimming a pale sleeve towards black leaves a grey
			// wash -- the one thing a frost must not be, and the worst ground
			// for light type, which wants a dark ground of SOME colour rather
			// than a light ground of none. So what the dim takes out of the
			// light, the tint puts back into the hue: each channel is pulled
			// away from the pixel's own grey by a fixed few per cent, leaving
			// the luminance the dim chose and the colour the sleeve has.
			int luma = (r * 299 + g * 587 + b * 114) / 1000;
			r = luma + ((r - luma) * BACKDROP_BAND_SAT_PCT) / 100;
			g = luma + ((g - luma) * BACKDROP_BAND_SAT_PCT) / 100;
			b = luma + ((b - luma) * BACKDROP_BAND_SAT_PCT) / 100;
		}
		rgb[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
		rgb[i + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
		rgb[i + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
	}

	bool ok = pack_rgb565_dithered(rgb, box_w, box_h, out);
	free(rgb);
	return ok;
}

// A blurred, dimmed copy of a picture that has already been scaled and packed.
//
// Not make_backdrop() with different numbers: that one starts from the decoded
// source and is built while the track is being loaded, because it is wanted on
// every track. This is asked for from the sleeve already on screen, at the
// moment the VU meters are opened, and by then the decoded source has long been
// freed.
//
// Same treatment as the backdrop, so the sleeve behind the needles and the
// block behind the controls read as one surface: shrink, box-blur, stretch,
// and the same push away from the text colour.
// The one colour that stands for a sleeve: the pool of light under the record
// in Cover Flow, and what the player's alternative layout tints itself with.
//
// A plain average of a cover comes out mud: every sleeve has a lot of near-grey
// in it and the greys drown the one colour a person would name. So each pixel
// is weighted by how far it is from grey, and the near-greys count for almost
// nothing. What comes back is then pushed towards full saturation, because a
// glow is light and not paint: a washed-out tint reads as a smudge on the
// screen rather than as something lit.
//
// Every fourth pixel each way, which is a sixteenth of the work for a number
// that does not change in the last decimal.
//
// Two things here keep a black-and-white sleeve from coming out green.
//
// The first is how a 565 pixel is opened out. Shifting the five- and six-bit
// fields up by three and two leaves green a step ahead of red and blue on every
// grey there is: white arrives as 248,252,248 and mid grey as 96,100,96 -- four
// units of pure green, which the stretch below takes to full strength.
// Replicating the top bits down into the gap instead makes white 255,255,255
// and leaves the rest off by a unit or two either way, with no colour
// preferred.
//
// The second is the floor. Two units of rounding noise in a grey average is
// still a hue, and stretching it produces a confident colour out of nothing. So
// a sleeve whose average is that close to grey is taken at its word and lights
// the screen white.
#define TONE_CHROMA_FLOOR 14
#define TONE_MONO 0xF2EEE8 // barely warm white: light, not paint

uint32_t cover_dominant_tone(const cover_image_t *cover) {
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	int stride = (int)cover->dsc.header.stride / 2;
	if (!cover->pixels || w <= 0 || h <= 0) {
		return 0;
	}

	uint64_t ar = 0, ag = 0, ab = 0, weight = 0;
	for (int y = 0; y < h; y += 4) {
		const uint16_t *row = (const uint16_t *)cover->pixels + (size_t)y * stride;
		for (int x = 0; x < w; x += 4) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
			unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
			unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
			// Distance from grey, and a little for being bright at all: a sleeve
			// that really is black and white still has to light something.
			unsigned k = (hi - lo) * 4 + hi / 8 + 1;
			ar += (uint64_t)r * k;
			ag += (uint64_t)g * k;
			ab += (uint64_t)b * k;
			weight += k;
		}
	}
	if (!weight) {
		return 0;
	}
	unsigned r = (unsigned)(ar / weight), g = (unsigned)(ag / weight), b = (unsigned)(ab / weight);

	// Pull it away from grey: the gap between the strongest channel and the
	// weakest is stretched, and the middle one moves with it, so the hue stays
	// where it was and only the strength changes.
	unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (hi - lo < TONE_CHROMA_FLOOR) {
		return TONE_MONO; // no colour worth finding: this sleeve is grey
	}
	if (hi > lo) {
		unsigned reach = hi > 200 ? 200 : hi; // a dark average is not stretched to full
		r = lo + (r - lo) * 255 / (hi - lo) * reach / 200;
		g = lo + (g - lo) * 255 / (hi - lo) * reach / 200;
		b = lo + (b - lo) * 255 / (hi - lo) * reach / 200;
	}
	if (r > 255) {
		r = 255;
	}
	if (g > 255) {
		g = 255;
	}
	if (b > 255) {
		b = 255;
	}

	// And lift the whole thing if it is still dark, so a black sleeve throws a
	// faint light of its own colour instead of a shadow. Taken after the
	// stretch above, which has usually already done most of it.
	hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (hi > 0 && hi < 180) {
		r = r * 180 / hi;
		g = g * 180 / hi;
		b = b * 180 / hi;
	}
	return (r << 16) | (g << 8) | b;
}

bool cover_blur_copy(const cover_image_t *src, cover_image_t *out) {
	if (!src) {
		if (out) {
			memset(out, 0, sizeof(*out));
		}
		return false;
	}
	return cover_blur_fill(src, (int)src->dsc.header.w, (int)src->dsc.header.h, out);
}

bool cover_blur_fill(const cover_image_t *src, int out_w, int out_h, cover_image_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!src || !src->pixels) {
		return false;
	}

	int w = (int)src->dsc.header.w;
	int h = (int)src->dsc.header.h;
	int stride = (int)src->dsc.header.stride / 2;
	if (w < 2 || h < 2 || stride < w) {
		return false;
	}

	// Back out to RGB888: the blur and the resampler both work there, and five
	// and six bit channels have nowhere to put the intermediate values.
	uint8_t *rgb = malloc((size_t)w * h * 3);
	if (!rgb) {
		return false;
	}
	for (int y = 0; y < h; y++) {
		const uint16_t *row = (const uint16_t *)src->pixels + (size_t)y * stride;
		uint8_t *drow = rgb + (size_t)y * w * 3;
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			drow[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
			drow[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
			drow[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
		}
	}

	if (out_w < 2 || out_h < 2) {
		free(rgb);
		return false;
	}
	raw_image_t whole_img = {.pixels = rgb, .w = w, .h = h};
	crop_t whole = crop_to_aspect(w, h, out_w, out_h);
	int small_w = out_w / BACKDROP_SCALE_DIVISOR;
	int small_h = out_h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2) {
		small_w = 2;
	}
	if (small_h < 2) {
		small_h = 2;
	}
	uint8_t *small = resample_rgb(&whole_img, &whole, small_w, small_h, false);
	free(rgb);
	if (!small) {
		return false;
	}

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		for (int i = 0; i < BACKDROP_BLUR_PASSES; i++) {
			box_blur_pass(small, scratch, small_w, small_h, BACKDROP_BLUR_RADIUS);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t tiny_whole = {0, 0, small_w, small_h};
	uint8_t *big = resample_rgb(&tiny, &tiny_whole, out_w, out_h, false);
	free(small);
	if (!big) {
		return false;
	}

	for (int i = 0; i < out_w * out_h * 3; i++) {
		if (backdrop_light) {
			big[i] = (uint8_t)(255 - ((255 - big[i]) * BACKDROP_BRIGHTNESS_PCT) / 100);
		} else {
			big[i] = (uint8_t)((big[i] * BACKDROP_BRIGHTNESS_PCT) / 100);
		}
	}

	bool ok = pack_rgb565_dithered(big, out_w, out_h, out);
	free(big);
	return ok;
}

// ---------------------------------------------------------------------------
// building the finished images
//
// Decoding is the one operation here that can ask for more memory than the
// device has. Everything downstream works on the scaled result and costs a few
// hundred kilobytes at most, so all the risk sits in a single call, which is
// what makes it worth isolating.
// ---------------------------------------------------------------------------

typedef struct {
	int w;
	int h;
	cover_fit_t fit;
	bool backdrop; // build the blurred, flipped copy instead of a plain scale
	// The backdrop comes from the BOTTOM band of the source instead of a
	// centred crop of it, and is not flipped. What a blurred block that sits
	// ON the picture wants -- the screensaver's foot, and the player's
	// Immersive deck: there the blur stands over the part of the photograph it
	// frosts, so it has to be a blurred continuation of exactly those pixels;
	// a mirrored centre crop would read as a different photograph.
	bool from_bottom;
	// The box the picture this band stands over was made at, with from_bottom:
	// the band is cut from that crop's own foot so its colours continue the
	// photograph above it (see make_backdrop). Zero when nobody asked for one.
	int over_w, over_h;
} image_request_t;

// Decodes once and produces every requested image from that one decode. The
// expensive part is the JPEG, not the scaling, so a track change asking for
// both the cover and its backdrop still only decodes once.
static bool build_images(const uint8_t *data, size_t size, const image_request_t *reqs, int n, cover_image_t *outs) {
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	// Nobody asked for anything. Without this the box below comes out zero, both
	// scaled decoders are skipped (they need a box to aim at) and the picture
	// goes through the unbounded path at full resolution -- the one thing this
	// file exists to avoid.
	if (n < 1) {
		return false;
	}

	// The largest edge anybody asked for decides how much resolution the decode
	// must keep; everything smaller is resampled from that.
	//
	// A backdrop is the exception. make_backdrop() reduces it to half before it
	// blurs, so the decode only has to satisfy that half -- the rest is thrown
	// away. Letting the backdrop's own size -- the screen -- decide the decode
	// made the sleeve pay for it: a 1400-pixel cover was decoded at 350 to
	// produce a 110-pixel sleeve and a 120x160 blur.
	int max_box = 0;
	for (int i = 0; i < n; i++) {
		int need = reqs[i].w > reqs[i].h ? reqs[i].w : reqs[i].h;
		if (reqs[i].backdrop) {
			need /= BACKDROP_SCALE_DIVISOR;
		}
		if (need < 1) {
			need = 1;
		}
		if (need > max_box)
			max_box = need;
	}

	raw_image_t raw;
	if (!decode_raw(data, size, max_box, &raw)) {
		return false;
	}

	bool first_ok = false;
	for (int i = 0; i < n; i++) {
		bool ok;
		if (reqs[i].backdrop) {
			ok = make_backdrop(&raw, reqs[i].w, reqs[i].h, reqs[i].from_bottom, reqs[i].over_w, reqs[i].over_h,
							   &outs[i]);
		} else {
			ok = make_scaled(&raw, reqs[i].w, reqs[i].h, reqs[i].fit, &outs[i]);
		}
		if (i == 0) {
			first_ok = ok;
		}
	}

	raw_free(&raw);
	return first_ok;
}

// ---------------------------------------------------------------------------
// bounded decoding
//
// No helper processes: the decode stays on the worker thread and is refused
// when it does not comfortably fit the memory actually free. The refusal is
// cheap (stbi_info reads only the header), the negative result is cached on disk
// like any other, and a folder whose cover is genuinely too big shows a glyph
// instead of gambling the whole player on an optimistic malloc.
//
// Isolating the decode in a forked child is not an option here. fork() on a
// single 1.2 GHz core copies the page tables of a ~25 MB process while holding
// mmap_sem, stalling every other thread's page faults, and fork() from a
// multithreaded process can catch another thread holding the malloc lock,
// leaving the child deadlocked in its first allocation.
// ---------------------------------------------------------------------------

static bool build_images_safely(const uint8_t *data, size_t size, const image_request_t *reqs, int n,
								cover_image_t *outs) {
	// The budget guard lives inside decode_raw and applies to the stb path only:
	// baseline JPEGs go through the scaled decoder, whose memory use is set by
	// the requested output rather than the source, so a poster-sized cover needs
	// no refusing at all.
	return build_images(data, size, reqs, n, outs);
}

// ---------------------------------------------------------------------------
// Walking the sources
//
// Finding artwork and decoding it are two different ways to fail, and the
// difference matters twice over.
//
// A track can carry a picture the decoder will not take -- a progressive JPEG
// well past the memory budget, an APIC truncated by a bad tagger. Committing to
// the first bytes found would leave that track showing nothing even with a
// perfectly good cover.jpg beside it, so every source is tried in turn until
// one decodes.
//
// And a decode that was refused is not the same answer as "there is no artwork
// here". The first depends on how much memory happened to be free; the second
// is a property of the files. Only the second is worth remembering on disk (see
// the thumbnail cache), which is why the outcome is reported as three states
// rather than a bool.
// ---------------------------------------------------------------------------

typedef enum {
	COVER_BUILD_OK,		// an image was produced
	COVER_BUILD_NONE,	// no source carried a picture
	COVER_BUILD_FAILED, // a picture was found, none of them would decode
} cover_build_t;

static cover_build_t build_from_sources(const char *path, bool is_dir, const image_request_t *reqs, int n,
										cover_image_t *outs) {
	bool had_bytes = false;

	// Noted for the crash handler and cleared on the way out, so a log that
	// still names a file says the fault landed inside the decode rather than
	// merely at the same moment as one -- which is as far as the log line
	// printed below can settle it, since that one comes from this thread while
	// the interface runs on another.
	crumb_set_artwork(path);

	// Zeroed here rather than only inside build_images: a path with no artwork
	// at all never reaches that function, and the caller still reads these.
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		bool loaded = is_dir ? albumart_load_dir_candidate(path, i, &art) : albumart_load_candidate(path, i, &art);
		if (!loaded) {
			continue;
		}

		had_bytes = true;
		bool ok = build_images_safely(art.data, art.size, reqs, n, outs);
		albumart_free(&art);
		if (ok) {
			crumb_set_artwork(NULL);
			return COVER_BUILD_OK;
		}

		// A build reports on its first image only, so a failure can still have
		// left a backdrop behind. Clear every slot before the next candidate
		// overwrites the pointers.
		for (int j = 0; j < n; j++) {
			cover_free(&outs[j]);
		}
	}

	crumb_set_artwork(NULL);
	return had_bytes ? COVER_BUILD_FAILED : COVER_BUILD_NONE;
}

// ---------------------------------------------------------------------------
// public loaders
// ---------------------------------------------------------------------------

bool cover_load_for_file(const char *filepath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(filepath, false, &req, 1, out) == COVER_BUILD_OK;
}

bool cover_load_for_dir(const char *dirpath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(dirpath, true, &req, 1, out) == COVER_BUILD_OK;
}

// A picture that is simply a file, with no track and no folder behind it.
// AirPlay is the caller: shairport writes the artwork the phone sent to
// /tmp/cover-<md5>.jpg and reports the path, so there is nothing to look up.
bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!path || !path[0])
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}

	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (got != (size_t)size) {
		free(data);
		return false;
	}

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	bool ok = build_images_safely(data, got, &req, 1, out);
	free(data);
	return ok;
}

// The same decode as cover_load_image_file(), but left as RGB888: a caller
// that is going to encode the picture again (the Boot screen centre) cannot
// use the RGB565 the display draws, and going through it would throw away
// colour depth for no reason.
uint8_t *cover_load_image_rgb(const char *path, int box_w, int box_h, cover_fit_t fit, int *out_w, int *out_h) {
	if (out_w)
		*out_w = 0;
	if (out_h)
		*out_h = 0;

	if (!path || !path[0] || box_w < 1 || box_h < 1)
		return NULL;

	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return NULL;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (got != (size_t)size) {
		free(data);
		return NULL;
	}

	int max_box = box_w > box_h ? box_w : box_h;
	raw_image_t raw;
	if (!decode_raw(data, got, max_box, &raw)) {
		free(data);
		return NULL;
	}
	free(data);

	crop_t crop;
	int dst_w, dst_h;
	scaled_geometry(&raw, box_w, box_h, fit, &crop, &dst_w, &dst_h);

	uint8_t *rgb = resample_rgb(&raw, &crop, dst_w, dst_h, false);
	raw_free(&raw);
	if (!rgb)
		return NULL;

	if (out_w)
		*out_w = dst_w;
	if (out_h)
		*out_h = dst_h;
	return rgb;
}


bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *image_out, int strip_w,
								  int strip_h, cover_image_t *strip_out) {
	if (image_out)
		memset(image_out, 0, sizeof(*image_out));
	if (strip_out)
		memset(strip_out, 0, sizeof(*strip_out));
	if (!path || !path[0] || !image_out || !strip_out)
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}
	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(data);
		return false;
	}

	// One decode, two pictures, as on the player screen -- except that the
	// blurred one comes from the bottom of the same photograph rather than from
	// the middle of it.
	image_request_t reqs[2] = {
		{.w = w, .h = h, .fit = COVER_FIT_COVER, .backdrop = false, .from_bottom = false},
		{.w = strip_w, .h = strip_h, .fit = COVER_FIT_COVER, .backdrop = true, .from_bottom = true},
	};
	cover_image_t results[2];
	bool ok = build_images_safely(data, got, reqs, 2, results);
	free(data);

	if (!ok) {
		cover_free(&results[0]);
		cover_free(&results[1]);
		return false;
	}
	*image_out = results[0];
	*strip_out = results[1];
	return true;
}

// The size, with four kilobytes from each end folded in. Not the whole picture:
// hashing four megabytes on this processor would cost more than the question is
// worth, and two different covers that agree on their length and on both ends
// do not happen outside a deliberate attempt.
static uint64_t bytes_id(const uint8_t *data, size_t size) {
	uint64_t h = 1469598103934665603ULL;
	h ^= (uint64_t)size;
	h *= 1099511628211ULL;

	size_t edge = size < 4096 ? size : 4096;
	for (size_t i = 0; i < edge; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	for (size_t i = size - edge; i < size; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	// Never zero: that is the answer for "no artwork", and a picture that
	// hashed to it would read as one that is not there.
	return h ? h : 1;
}

uint64_t cover_source_id(const char *filepath) {
	if (!filepath || !filepath[0]) {
		return 0;
	}
	// The first source that has bytes, which is the one the decode will start
	// from. It may yet fail to decode and send the build on to the next
	// candidate -- but that outcome follows from these same bytes, so two
	// tracks that agree here agree about the picture that ends up on screen.
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		if (!albumart_load_candidate(filepath, i, &art)) {
			continue;
		}
		uint64_t id = art.size ? bytes_id(art.data, art.size) : 0;
		albumart_free(&art);
		if (id) {
			return id;
		}
	}
	return 0;
}

// Stored under the same key the scan's artwork pass writes (defined with the
// cache below).
static void thumb_store_player(const char *path, int size, const cover_image_t *img);

bool cover_load_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out) {
	if (cover_out)
		memset(cover_out, 0, sizeof(*cover_out));
	if (backdrop_out)
		memset(backdrop_out, 0, sizeof(*backdrop_out));

	// A square sleeve is one of the entries the scan pass rendered: same file,
	// same size, same crop. Reading it costs one small blob, and the backdrop can
	// be made from the picture itself, so a track change needs no decoder at all.
	//
	// Skipped when the backdrop is a band (it is cut from the foot of the source
	// and tinted, which cover_blur_fill() does not do) and when the backdrop is
	// meant to be a flipped reflection (make_backdrop() flips it, the blur copy
	// does not).
	bool square = cover_out && cover_w > 0 && cover_w == cover_h;
	bool plain_backdrop = !backdrop_out || (backdrop_upright && !backdrop_from_bottom);
	if (square && plain_backdrop) {
		cover_image_t cached;
		if (cover_thumb_cached_fit(filepath, cover_w, true, &cached)) {
			*cover_out = cached;
			if (backdrop_out) {
				cover_blur_fill(&cached, backdrop_w, backdrop_h, backdrop_out);
			}
			return true;
		}
		// Said out loud because a miss here is the difference between a track
		// change that costs a read and one that costs the decode logged below.
		// The size is the key it looked for, which is what a scan that rendered
		// at another size gets wrong.
		fprintf(stderr, "cover: sleeve %dpx not in the cache, decoding\n", cover_w);
	} else if (cover_out) {
		fprintf(stderr, "cover: sleeve %dx%d is not square, so it is decoded every time\n", cover_w, cover_h);
	}

	// One decode feeds both images: the expensive part is the JPEG, not the
	// scaling, so a track change decodes once rather than twice.
	image_request_t reqs[2] = {0}; // a caller may want neither image
	cover_image_t results[2];
	int n = 0;
	int cover_index = -1, backdrop_index = -1;

	if (cover_out) {
		cover_index = n;
		reqs[n++] = (image_request_t){.w = cover_w, .h = cover_h, .fit = COVER_FIT_COVER, .backdrop = false};
	}
	if (backdrop_out) {
		backdrop_index = n;
		// With from_bottom the band is cut out of the cover's own crop (see
		// make_backdrop), so the cover's box travels with the request; without
		// one asked for there is nothing to continue and the band takes the
		// foot of the source whole.
		reqs[n++] = (image_request_t){.w = backdrop_w, .h = backdrop_h, .fit = COVER_FIT_COVER, .backdrop = true,
									  .from_bottom = backdrop_from_bottom,
									  .over_w = backdrop_from_bottom && cover_out ? cover_w : 0,
									  .over_h = backdrop_from_bottom && cover_out ? cover_h : 0};
	}

	cover_build_t built = build_from_sources(filepath, false, reqs, n, results);
	if (built == COVER_BUILD_NONE) {
		// A podcast episode with no cover file beside it: fetch it now, on the
		// loader thread, which can afford to wait on a network request, then
		// try again. This covers episodes started from the queue, whose tags
		// carry the cover URL but whose jpg was never downloaded.
		if (!podcastcache_ensure_cover(filepath)) {
			return false;
		}
		built = build_from_sources(filepath, false, reqs, n, results);
	}
	bool ok = (built == COVER_BUILD_OK);

	// A sleeve decoded here is the same picture the scan pass would have written,
	// so keep it: the next track of this album costs a read, and a scan that has
	// already run and skipped this one is filled in.
	if (ok && square && cover_index >= 0 && results[cover_index].pixels) {
		thumb_store_player(filepath, cover_w, &results[cover_index]);
	}

	// A missing backdrop is survivable; the controls just stay plain.
	if (cover_index >= 0)
		*cover_out = results[cover_index];
	if (backdrop_index >= 0)
		*backdrop_out = results[backdrop_index];

	return ok;
}

void cover_free(cover_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

// ---------------------------------------------------------------------------
// on-disk thumbnail cache
//
// The same mechanism the stock player uses: resized artwork is kept on the card
// so browsing a library never decodes the original twice. What is stored is the
// already-scaled RGB565 thumbnail: a 44x44 entry is under 4 KB and loading one
// is a single read, with no decoder involved. The first pass over a big folder
// decodes, every pass after it reads back a few kilobytes. "No artwork here" is
// stored too, so empty folders stop being rescanned as well.
//
// A database, not a directory of one file per entry: thousands of small files in
// one directory is the worst thing to ask of a FAT filesystem, where listing and
// cleaning up are both slow and every entry costs a directory entry. It is a
// single SQLite file named thumbnails under .local, alongside library.db. The
// key is a hash of path + size + mtime + source size, so replacing a cover
// invalidates its entry by itself; the value is the pixels, or an empty row
// meaning nothing was found there.
// ---------------------------------------------------------------------------

// Only thumbnails are worth keeping on disk. The player's sleeve is one of
// them now: at 110-240 pixels it is 24-115 KB, and rendering it once per album
// is what makes a track change a read instead of a decode.
//
// The largest arrangement sets the ceiling: the sleeve at the full width of a
// 240-pixel panel. Anything larger is decoded per track, not cached.
#define THUMB_DISK_MAX_SIZE 240
#define THUMB_WAV_EPOCH "|w2"

static sqlite3 *thumb_db;
// The thumbnail worker writes while the GUI thread may be reading, and a card
// change closes and reopens the database under both: one connection, guarded by
// this lock.
static pthread_mutex_t thumb_db_lock = PTHREAD_MUTEX_INITIALIZER;

// Where the open cache came from, and which file it was, so that a cache
// deleted from under it is noticed (see cover_cache_reopen_if_replaced).
static char thumb_db_base[512];
static char thumb_db_path[512];
static file_identity_t thumb_db_identity;

// Everything the player keeps beside the user's music lives under one hidden
// directory, so a card ends up with a single .local folder rather than one dot
// entry per feature.
#define THUMB_DB_SUBPATH ".local/thumbnails"

// An earlier release kept the cache as a directory at the path the database now
// wants, so on first open it is emptied and removed. Everything inside it was
// written by this player (16 hex-digit names), and the content is rebuilt in the
// database on the first pass over the lists.
static void thumb_wipe_old_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	char victim[600];
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		if (snprintf(victim, sizeof(victim), "%s/%s", dir, e->d_name) < (int)sizeof(victim)) {
			unlink(victim);
		}
	}
	closedir(d);
	rmdir(dir);
	printf("cover: old folder cache removed (%s)\n", dir);
}

// Schema plus pragma. synchronous=OFF because this is only a cache: an entry
// lost to a card pulled mid-write recomputes itself, and every fsync saved is
// smoother scrolling.
static bool thumb_db_prepare_schema(sqlite3 *db) {
	char *err = NULL;
	if (sqlite3_exec(db,
					 "PRAGMA synchronous=OFF;"
					 // SQLite's own default is 2 MB of page cache per
					 // connection, which browsing one big folder fills and
					 // keeps filled for the rest of the session. Every other
					 // database in the player (library, audiobooks,
					 // subscriptions, radio) asks for 64 to 256 kB.
					 "PRAGMA cache_size=-256;"
					 "CREATE TABLE IF NOT EXISTS thumbs("
					 " key TEXT PRIMARY KEY,"
					 " w INTEGER NOT NULL,"
					 " h INTEGER NOT NULL,"
					 " pixels BLOB)"
					 // The parsed tags of a track, in the same file and under the
					 // same key function as the pictures (metadata.h explains why
					 // they are worth keeping). A table of its own because the
					 // rows look nothing alike.
					 ";CREATE TABLE IF NOT EXISTS track_meta("
					 " key TEXT PRIMARY KEY,"
					 " blob BLOB)",
					 NULL, NULL, &err) == SQLITE_OK) {
		return true;
	}
	fprintf(stderr, "cover: thumbnails db refused: %s\n", err ? err : "?");
	sqlite3_free(err);
	return false;
}

// Opens or creates <base>/.local/thumbnails and adopts it. Call with the lock
// already held.
static bool try_cache_db(const char *base) {
	char parent[512];
	char path[512];

	// A base path long enough to overflow these would leave a truncated name,
	// which is worse than simply having no cache.
	if (snprintf(parent, sizeof(parent), "%s/.local", base) >= (int)sizeof(parent)) {
		return false;
	}
	mkdir(parent, 0755);

	if (snprintf(path, sizeof(path), "%s/%s", base, THUMB_DB_SUBPATH) >= (int)sizeof(path)) {
		return false;
	}

	// The old cache directory occupies the same name as the database file, so
	// clear it out of the way first.
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		thumb_wipe_old_dir(path);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			return false; // it would not go away, so no cache here
		}
	}

	sqlite3 *db = NULL;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "cover: cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return false;
	}

	if (!thumb_db_prepare_schema(db)) {
		// A corrupt database, typically a card pulled mid-write, does not heal:
		// delete the file and start clean, since it is only a cache.
		sqlite3_close(db);
		unlink(path);
		db = NULL;
		if (sqlite3_open(path, &db) != SQLITE_OK || !thumb_db_prepare_schema(db)) {
			sqlite3_close(db);
			return false;
		}
	}

	thumb_db = db;
	snprintf(thumb_db_base, sizeof(thumb_db_base), "%s", base);
	snprintf(thumb_db_path, sizeof(thumb_db_path), "%s", path);
	file_identity_read(path, &thumb_db_identity);
	printf("cover: thumbnail cache in %s\n", path);
	return true;
}

void cover_cache_reopen_if_replaced(void) {
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db && thumb_db_path[0] && file_identity_changed(thumb_db_path, &thumb_db_identity)) {
		printf("cover: %s was deleted or replaced; opening it again\n", thumb_db_path);
		sqlite3_close(thumb_db);
		thumb_db = NULL;
		char base[sizeof(thumb_db_base)];
		snprintf(base, sizeof(base), "%s", thumb_db_base);
		if (!try_cache_db(base) && !try_cache_db("/tmp")) {
			printf("cover: thumbnail cache disabled\n");
		}
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// The heavy app tier: the page cache goes back to the kernel, the connection
// and the card side of the cache stay as they are.
void cover_cache_release_memory(void) {
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db) {
		sqlite3_db_release_memory(thumb_db);
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// Defined with the RAM cache further down; the keys are path based, so moving
// the cache invalidates every entry.
static void ram_clear(void);

void cover_set_cache_dir(const char *sd_root) {
	pthread_mutex_lock(&thumb_db_lock);

	if (thumb_db) {
		sqlite3_close(thumb_db);
		thumb_db = NULL;
	}

	bool ok = sd_root && sd_root[0] && try_cache_db(sd_root);
	if (!ok) {
		// The card may be missing or read-only; a tmpfs cache still saves the
		// rescan cost within a single run.
		ok = try_cache_db("/tmp");
	}

	pthread_mutex_unlock(&thumb_db_lock);

	ram_clear();

	if (!ok) {
		printf("cover: thumbnail cache disabled\n");
	}
	fprintf(stderr, "cover: %zu KB free, decode budget %zu KB\n", read_mem_available() / 1024,
			decode_budget_bytes() / 1024);
}

// FNV-1a. The cache key has to change when the source does, so the file's size
// and timestamp go into it: replacing a cover.jpg invalidates the thumbnail
// without anybody having to clear the cache.
static uint64_t thumb_hash(const char *path, int box, bool is_dir, bool fit_cover) {
	struct stat st;
	uint64_t h = 1469598103934665603ULL;

	// THUMB_WAV_EPOCH goes into the key of every folder and every .wav: both
	// were once answered "no artwork" for a WAV that has it in its ID3 chunk,
	// and that answer is on disk under keys nothing else would change.
	const char *epoch = (is_dir || has_extension(path, ".wav")) ? THUMB_WAV_EPOCH : "";

	char material[700];
	// The fit is part of the key only when it is the player's crop-to-fill: a
	// list thumbnail fits the whole picture inside its square, a sleeve crops it.
	// Leaving the contain form's material exactly as it was keeps every entry
	// already on the card valid.
	const char *fit = fit_cover ? "|c" : "";
	if (stat(path, &st) == 0) {
		snprintf(material, sizeof(material), "%s|%d|%c|%lld|%lld%s%s", path, box, is_dir ? 'd' : 'f',
				 (long long)st.st_mtime, (long long)st.st_size, epoch, fit);
	} else {
		snprintf(material, sizeof(material), "%s|%d|%c|?%s%s", path, box, is_dir ? 'd' : 'f', epoch, fit);
	}

	for (const char *p = material; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

// Builds the cache key. False means this size is not worth caching. Whether the
// database is open at all is decided under the lock, in load and store.
// `value_out`, when not NULL, receives the same key as a number, which is what
// the RAM cache compares (a string compare per slot would not be worth it).
static bool thumb_key(char *out, size_t out_size, uint64_t *value_out, const char *path, int box, bool is_dir,
					  bool fit_cover) {
	if (box > THUMB_DISK_MAX_SIZE) {
		return false;
	}
	uint64_t h = thumb_hash(path, box, is_dir, fit_cover);
	snprintf(out, out_size, "%016llx", (unsigned long long)h);
	if (value_out) {
		*value_out = h;
	}
	return true;
}

// Reads a cached thumbnail. Returns false on a miss; on a hit, has_image tells
// whether there was artwork at all.
static bool thumb_db_load(const char *key, cover_image_t *out, bool *has_image) {
	bool hit = false;

	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return false;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "SELECT w, h, pixels FROM thumbs WHERE key = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			int w = sqlite3_column_int(stmt, 0);
			int h = sqlite3_column_int(stmt, 1);

			if (w == 0 || h == 0) {
				*has_image = false; // cached "no artwork here"
				hit = true;
			} else if (w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF) {
				size_t bytes = (size_t)w * h * 2;
				const void *blob = sqlite3_column_blob(stmt, 2);
				// A row whose blob size does not match its dimensions comes from an
				// interrupted write and counts as a miss: decode again and
				// overwrite it.
				if (blob && (size_t)sqlite3_column_bytes(stmt, 2) == bytes) {
					uint8_t *pixels = malloc(bytes);
					if (pixels) {
						memcpy(pixels, blob, bytes);
						memset(out, 0, sizeof(*out));
						out->pixels = pixels;
						out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
						out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
						out->dsc.header.w = (uint32_t)w;
						out->dsc.header.h = (uint32_t)h;
						out->dsc.header.stride = (uint32_t)w * 2;
						out->dsc.data_size = (uint32_t)bytes;
						out->dsc.data = pixels;
						*has_image = true;
						hit = true;
					}
				}
			}
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

// A single write, made atomic by SQLite: no more .tmp file and rename, since
// the database journal does the same job.
static void thumb_db_store(const char *key, const cover_image_t *img, bool has_image) {
	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "INSERT OR REPLACE INTO thumbs(key, w, h, pixels) VALUES(?1, ?2, ?3, ?4)", -1,
						   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (has_image && img->pixels) {
			int w = (int)img->dsc.header.w;
			int h = (int)img->dsc.header.h;
			sqlite3_bind_int(stmt, 2, w);
			sqlite3_bind_int(stmt, 3, h);
			sqlite3_bind_blob(stmt, 4, img->pixels, (int)((size_t)w * h * 2), SQLITE_STATIC);
		} else {
			sqlite3_bind_int(stmt, 2, 0);
			sqlite3_bind_int(stmt, 3, 0);
			sqlite3_bind_null(stmt, 4);
		}
		sqlite3_step(stmt); // a failure here only costs a future re-decode
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
}

// Opens or closes a write transaction the following stores join. The scan's
// artwork pass writes a few hundred entries; one commit each is a journal
// round-trip on the card for every one of them. Batching by a small number
// keeps the cost down without risking much on a crash -- and the pass is
// resumable anyway, since an entry already written is a cache hit next time.
static void thumb_db_batch(bool begin) {
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db) {
		sqlite3_exec(thumb_db, begin ? "BEGIN" : "COMMIT", NULL, NULL, NULL);
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// ---------------------------------------------------------------------------
// The parsed tags of a track, in the same file as the pictures
//
// metadata.h explains why they are worth keeping. What matters here is that the
// key is the artwork's key: same function, same file size and timestamp, so an
// entry goes stale at exactly the moment a picture of the same track would.
//
// Box zero is the mark that says "tags": every picture is asked for at a box of
// at least one pixel, so no picture was ever written under it.
// ---------------------------------------------------------------------------

static bool meta_key(char *out, size_t out_size, const char *path) {
	return thumb_key(out, out_size, NULL, path, 0, false, false);
}

bool metadata_cache_load(const char *filepath, song_metadata_t *out) {
	if (!filepath || !filepath[0] || !out) {
		return false;
	}

	char key[32];
	if (!meta_key(key, sizeof(key), filepath)) {
		return false;
	}

	bool hit = false;
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(thumb_db, "SELECT blob FROM track_meta WHERE key=?1", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const void *blob = sqlite3_column_blob(stmt, 0);
				int size = sqlite3_column_bytes(stmt, 0);
				// The struct itself, so a build that adds a field finds every
				// old entry the wrong length and reads nothing out of it.
				if (blob && size == (int)sizeof(*out)) {
					memcpy(out, blob, sizeof(*out));
					hit = true;
				}
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

// A scan parses every track it meets, and one commit each would be a journal
// round-trip per track -- the very cost this table exists to avoid, moved into
// the scan. The database is opened synchronous=OFF, so a commit is cheap, but
// not free: they are grouped.
#define META_STORE_BATCH 64
static int meta_uncommitted;

void metadata_cache_store(const char *filepath, const song_metadata_t *meta) {
	if (!filepath || !filepath[0] || !meta) {
		return;
	}

	char key[32];
	if (!meta_key(key, sizeof(key), filepath)) {
		return;
	}

	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db) {
		// Joined rather than opened if something else is already writing --
		// thumb_db_batch() above is the only other, and never at the same
		// time, but a nested BEGIN is refused and asking is cheaper than
		// knowing. A handle that is not in a transaction is also how a reopened
		// cache starts, so the count goes back to zero with it.
		if (sqlite3_get_autocommit(thumb_db)) {
			meta_uncommitted = 0;
			sqlite3_exec(thumb_db, "BEGIN", NULL, NULL, NULL);
		}

		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(thumb_db, "INSERT OR REPLACE INTO track_meta(key, blob) VALUES(?1, ?2)", -1, &stmt,
							   NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
			sqlite3_bind_blob(stmt, 2, meta, (int)sizeof(*meta), SQLITE_TRANSIENT);
			sqlite3_step(stmt); // a failure only costs a future re-read
			sqlite3_finalize(stmt);
		}

		if (++meta_uncommitted >= META_STORE_BATCH) {
			sqlite3_exec(thumb_db, "COMMIT", NULL, NULL, NULL);
			meta_uncommitted = 0;
		}
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

void metadata_cache_flush(void) {
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db && meta_uncommitted > 0) {
		sqlite3_exec(thumb_db, "COMMIT", NULL, NULL, NULL);
		meta_uncommitted = 0;
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// ---------------------------------------------------------------------------
// A small RAM cache in front of the card
//
// The disk cache is one SQLite read, but it is still a read from the card: a
// list row measured at about 16 ms, so a screenful took long enough to notice
// even when every entry was there. Recently served thumbnails are kept here,
// so scrolling back over an album list -- and any repeat visit -- costs a
// memcpy instead.
//
// The entries are the finished RGB565 buffers, the same bytes the disk holds.
// Bounded twice: by bytes, so the cache cannot grow with the library, and by
// entry size, so a carousel-sized picture cannot evict a screenful of list
// thumbnails.
#define THUMB_RAM_BYTES (384 * 1024)
#define THUMB_RAM_ENTRIES 192
// Large enough for the player's sleeve at the compact layout's size (about
// 24 KB), so replaying an album does not go to the card at all.
#define THUMB_RAM_MAX_ENTRY (32 * 1024)

typedef struct {
	uint64_t key;
	uint64_t stamp;
	uint32_t bytes;
	uint16_t w, h;
	uint8_t *pixels;
} ram_thumb_t;

static ram_thumb_t ram_thumbs[THUMB_RAM_ENTRIES];
static size_t ram_bytes;
static uint64_t ram_clock;
static pthread_mutex_t ram_lock = PTHREAD_MUTEX_INITIALIZER;

// Set while the artwork pass runs. The pass walks the album list in display
// order, so the entries it writes first are the ones at the top of the list --
// the ones somebody opening the page is about to look at. Evicting in list
// order would leave the *last* albums in RAM and push the first ones back to
// the card, which is exactly backwards. In this mode the cache fills and then
// stops; the pass's own reads are already fast, so nothing is lost.
static volatile bool ram_fill_only;

// Caller holds ram_lock. Frees the least recently used entry.
static void ram_evict_oldest(void) {
	ram_thumb_t *oldest = NULL;
	for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
		if (!ram_thumbs[i].pixels) {
			continue;
		}
		if (!oldest || ram_thumbs[i].stamp < oldest->stamp) {
			oldest = &ram_thumbs[i];
		}
	}
	if (!oldest) {
		return;
	}
	ram_bytes -= oldest->bytes;
	free(oldest->pixels);
	oldest->pixels = NULL;
	oldest->bytes = 0;
}

// A copy of the cached picture, or false. A copy rather than a pointer into
// the cache: the caller owns what it is given and frees it, and a slot may be
// evicted while it is on screen.
static bool ram_get(uint64_t key, cover_image_t *out) {
	bool hit = false;

	pthread_mutex_lock(&ram_lock);
	for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
		ram_thumb_t *e = &ram_thumbs[i];
		if (!e->pixels || e->key != key) {
			continue;
		}

		uint8_t *copy = malloc(e->bytes);
		if (copy) {
			memcpy(copy, e->pixels, e->bytes);
			memset(out, 0, sizeof(*out));
			out->pixels = copy;
			out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
			out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
			out->dsc.header.w = e->w;
			out->dsc.header.h = e->h;
			out->dsc.header.stride = (uint32_t)e->w * 2;
			out->dsc.data_size = e->bytes;
			out->dsc.data = copy;
			e->stamp = ++ram_clock;
			hit = true;
		}
		break;
	}
	pthread_mutex_unlock(&ram_lock);

	return hit;
}

static void ram_put(uint64_t key, const cover_image_t *img) {
	size_t bytes = (size_t)img->dsc.header.w * img->dsc.header.h * 2;
	if (!img->pixels || bytes == 0 || bytes > THUMB_RAM_MAX_ENTRY || bytes > THUMB_RAM_BYTES) {
		return;
	}

	pthread_mutex_lock(&ram_lock);

	// An entry under this key is stale: we only reach here after a fresh read
	// or decode.
	for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
		if (ram_thumbs[i].pixels && ram_thumbs[i].key == key) {
			ram_bytes -= ram_thumbs[i].bytes;
			free(ram_thumbs[i].pixels);
			ram_thumbs[i].pixels = NULL;
			ram_thumbs[i].bytes = 0;
			break;
		}
	}

	while (ram_bytes + bytes > THUMB_RAM_BYTES) {
		if (ram_fill_only) {
			pthread_mutex_unlock(&ram_lock);
			return;
		}
		ram_evict_oldest();
	}

	int slot = -1;
	for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
		if (!ram_thumbs[i].pixels) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		if (ram_fill_only) {
			pthread_mutex_unlock(&ram_lock);
			return;
		}
		ram_evict_oldest();
		for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
			if (!ram_thumbs[i].pixels) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&ram_lock);
		return;
	}

	uint8_t *copy = malloc(bytes);
	if (!copy) {
		pthread_mutex_unlock(&ram_lock);
		return;
	}
	memcpy(copy, img->pixels, bytes);

	ram_thumb_t *e = &ram_thumbs[slot];
	e->pixels = copy;
	e->bytes = (uint32_t)bytes;
	e->w = (uint16_t)img->dsc.header.w;
	e->h = (uint16_t)img->dsc.header.h;
	e->key = key;
	e->stamp = ++ram_clock;
	ram_bytes += bytes;

	pthread_mutex_unlock(&ram_lock);
}

// The keys are built from the file path, so a different card makes every entry
// a lie. Called when the cache moves.
static void ram_clear(void) {
	pthread_mutex_lock(&ram_lock);
	for (int i = 0; i < THUMB_RAM_ENTRIES; i++) {
		free(ram_thumbs[i].pixels);
		ram_thumbs[i].pixels = NULL;
		ram_thumbs[i].bytes = 0;
	}
	ram_bytes = 0;
	pthread_mutex_unlock(&ram_lock);
}

// ---------------------------------------------------------------------------
// browser thumbnails
// ---------------------------------------------------------------------------

static cover_build_t thumb_load_uncached(const char *path, bool is_dir, int size, bool fit_cover, cover_image_t *out) {
	image_request_t req = {.w = size,
					   .h = size,
					   .fit = fit_cover ? COVER_FIT_COVER : COVER_FIT_CONTAIN,
					   .backdrop = false};
	return build_from_sources(path, is_dir, &req, 1, out);
}

// The path a thumbnail is keyed and decoded from. A track resolves to the first
// track of its album, which is the file an album row names; everything else
// (a directory, a picture, an audiobook) is used as given.
static const char *thumb_source_path(const char *path, bool is_dir, char *buf, size_t size) {
	if (!is_dir && path && library_album_art_path(path, buf, size)) {
		return buf;
	}
	return path;
}

bool cover_thumb_cached_fit(const char *path, int size, bool fit_cover, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	// Resolved like every other worker-side request, so a track asks for its
	// album's entry and not for one no scan pass ever wrote.
	char source[1024];
	const char *from = thumb_source_path(path, false, source, sizeof(source));

	char key[32];
	if (!thumb_key(key, sizeof(key), NULL, from, size, false, fit_cover)) {
		return false;
	}

	bool has_image = false;
	if (thumb_db_load(key, out, &has_image)) {
		return has_image;
	}
	return false;
}

bool cover_thumb_cached(const char *path, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	// No album resolution here. The only caller is the carousel, which passes
	// an album row's own path (that row's first track), so the key is already
	// the one the worker used -- and this runs on the interface thread, where
	// an extra query is the last thing to add.
	char key[32];
	uint64_t value = 0;
	if (!thumb_key(key, sizeof(key), &value, path, size, false, false)) {
		return false;
	}

	if (ram_get(value, out)) {
		return true;
	}

	bool has_image = false;
	if (thumb_db_load(key, out, &has_image)) {
		if (has_image) {
			ram_put(value, out);
		}
		return has_image;
	}
	return false;
}

bool cover_thumb_load_fit(const char *path, bool is_dir, int size, bool fit_cover, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	// A track is resolved to the first track of its *album* -- the same file an
	// album row names -- and both the key and the decode use that, so every
	// track of an album shares one entry and one decode. Using the folder
	// instead would break as soon as a folder held tracks from two albums;
	// using the album group does not, because the grouping comes from the tags.
	// Directories, pictures and anything the index does not know are used as
	// given.
	char source[1024];
	const char *from = thumb_source_path(path, is_dir, source, sizeof(source));

	char key[32];
	uint64_t value = 0;
	bool cacheable = thumb_key(key, sizeof(key), &value, from, size, is_dir, fit_cover);

	// RAM first: an entry already served costs a memcpy instead of a card read.
	// This is what makes scrolling back over a screen, and reopening a page,
	// look instant rather than merely quick. Only the list thumbnails are kept
	// in RAM: a sleeve is 24 KB against a list row's 2.6, and the player asks
	// for one of them per album, so keeping sleeves there would evict a
	// screenful of thumbnails to save a single read.
	if (cacheable && !fit_cover && ram_get(value, out)) {
		return true;
	}

	// Then disk: a hit costs one small read instead of a decode, which is what
	// makes the first look at an album cost nothing after the scan.
	if (cacheable) {
		bool has_image = false;
		if (thumb_db_load(key, out, &has_image)) {
			if (has_image && !fit_cover) {
				ram_put(value, out);
			}
			return has_image;
		}
	}

	cover_build_t built = thumb_load_uncached(from, is_dir, size, fit_cover, out);

	// "There is no artwork here" is worth storing too: a folder without any is
	// otherwise rescanned from scratch every time it scrolls past.
	//
	// A refused or failed decode is not stored. The decode budget is a quarter
	// of the memory free at that instant, so the same file can be refused now
	// and decode fine a minute later -- and the key is built from the file's own
	// size and mtime, so nothing would ever invalidate the entry. Storing a
	// failure would leave that track without a cover for good.
	if (cacheable && built != COVER_BUILD_FAILED) {
		thumb_db_store(key, out, built == COVER_BUILD_OK);
		if (built == COVER_BUILD_OK && !fit_cover) {
			ram_put(value, out);
		}
	}

	return built == COVER_BUILD_OK;
}

bool cover_thumb_load(const char *path, bool is_dir, int size, cover_image_t *out) {
	return cover_thumb_load_fit(path, is_dir, size, false, out);
}

// Stores the player's sleeve under the key cover_thumb_cached_fit() will look
// for later: the album's own file, the sleeve's size, and the crop-to-fill fit.
static void thumb_store_player(const char *path, int size, const cover_image_t *img) {
	char source[1024];
	const char *from = thumb_source_path(path, false, source, sizeof(source));

	char key[32];
	if (!thumb_key(key, sizeof(key), NULL, from, size, false, true)) {
		return; // too large to keep, or the path is not one we can key
	}

	thumb_db_store(key, img, true);
}

// ---------------------------------------------------------------------------
// Pre-rendering the album list
// ---------------------------------------------------------------------------

int cover_thumb_size(void) { return bp_pick(36, 72); }

// The square the player draws its sleeve in. Zero until the player has been
// laid out, or when the arrangement draws a rectangle.
static int player_cover_size;

void cover_set_player_cover_size(int size) { player_cover_size = size > 0 ? size : 0; }

// How many album paths are held at once while walking the index. Only the
// working set of one query, not of the library.
#define PRECACHE_WINDOW 16

// How many entries one transaction covers. The pass writes one row per album;
// committing each would be a journal round-trip on the card every time.
#define PRECACHE_COMMIT_EVERY 32

typedef struct {
	int count;
	char (*paths)[512];
} precache_batch_t;

static bool precache_row_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	precache_batch_t *batch = user;
	if (batch->count >= PRECACHE_WINDOW) {
		return false;
	}
	// An album row names the track its cover comes from; a row without one has
	// nothing to render.
	if (path && path[0]) {
		snprintf(batch->paths[batch->count++], 512, "%s", path);
	}
	return true;
}

int cover_precache_albums(bool (*stop)(void), void (*progress)(int done, int total, void *user), void *user) {
	// Without the database this would decode every cover and store nothing,
	// and every browse would pay for it again. Better to do nothing at all.
	pthread_mutex_lock(&thumb_db_lock);
	bool ready = thumb_db != NULL;
	pthread_mutex_unlock(&thumb_db_lock);
	if (!ready) {
		return -1;
	}

	// The same list the Albums page opens (music.c), so the entries land under
	// the keys its rows will ask for.
	library_index_t *ix = library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	if (!ix) {
		return -1;
	}

	int total = library_index_count(ix);
	if (total < 0) {
		total = 0;
	}

	precache_batch_t batch;
	batch.count = 0;
	batch.paths = malloc((size_t)PRECACHE_WINDOW * 512);
	if (!batch.paths) {
		library_index_close(ix);
		return -1;
	}

	int done = 0;
	int made = 0;
	int sleeves = 0;
	int offset = 0;
	uint64_t started = now_ms();

	// Shown before the first decode, so the page stops naming the last file the
	// scan read and says what is happening now.
	if (progress && total > 0) {
		progress(0, total, user);
	}

	// One image at a time with the progress page up: half of what is free
	// rather than a quarter. Without this a progressive cover that needs just
	// over the stricter budget is refused, not cached, and refused again every
	// time somebody opens its album.
	decode_budget_relaxed = true;
	// Fill the RAM cache in list order and then stop, so what survives is the
	// top of the list rather than the bottom.
	ram_fill_only = true;
	thumb_db_batch(true);

	while (offset < total) {
		if (stop && stop()) {
			break;
		}

		batch.count = 0;
		int got = library_index_window(ix, offset, PRECACHE_WINDOW, precache_row_cb, &batch);
		if (got <= 0) {
			break;
		}

		for (int i = 0; i < batch.count; i++) {
			if (stop && stop()) {
				break;
			}

			// Exactly what the list's worker asks for, so a hit here is a hit
			// there: same path, same size, same key.
			cover_image_t image;
			if (cover_thumb_load(batch.paths[i], false, cover_thumb_size(), &image)) {
				made++;
			}
			cover_free(&image);

			// The player's sleeve, so a track change reads instead of decoding.
			// A second picture rather than a free one: the two sizes can land on
			// different JPEG scales, so neither can be derived from the other by
			// the cache.
			if (player_cover_size > 0) {
				cover_image_t art;
				if (cover_thumb_load_fit(batch.paths[i], false, player_cover_size, true, &art)) {
					sleeves++;
				}
				cover_free(&art);
			}

			done++;
			if (progress) {
				progress(done, total, user);
			}

			if (done % PRECACHE_COMMIT_EVERY == 0) {
				thumb_db_batch(false);
				thumb_db_batch(true);
			}
		}

		offset += got;
	}

	thumb_db_batch(false);

	free(batch.paths);
	library_index_close(ix);
	decode_budget_relaxed = false;
	ram_fill_only = false;

	// The measurement that matters for deciding whether this stays in the scan:
	// what it cost, and how much of it was work at all rather than cache hits.
	fprintf(stderr, "cover: album artwork pass: %d rendered, %d sleeves, %d albums in %llu ms\n", made, sleeves, done,
			(unsigned long long)(now_ms() - started));
	return made;
}
