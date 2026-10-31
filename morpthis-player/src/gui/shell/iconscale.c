#include "src/gui/shell/iconscale.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/image/resample_fpu.h"

// How many scaled icons are kept and how many bytes of pixel data that may
// cost. A settings page holds around a dozen 18-24 px masks, which is a few
// kilobytes. Coloured menu art is much larger per icon, so it draws from a
// smaller budget of its own: whatever a menu page has taken, the masks of the
// scrolling lists behind it always have room. Running out is not an error: the
// icon is then drawn the old way.
#define ICONSCALE_SLOTS 96
#define ICONSCALE_BYTES ((size_t)256 * 1024)
#define ICONSCALE_BIG_ENTRY ((size_t)4 * 1024)
#define ICONSCALE_BIG_BYTES ((size_t)96 * 1024)

typedef struct {
	const lv_image_dsc_t *src;
	uint32_t w;
	uint32_t h;
	lv_image_dsc_t dsc;
} iconscale_entry_t;

static iconscale_entry_t entries[ICONSCALE_SLOTS];
static int entry_count;
static size_t bytes_used;
static size_t big_bytes_used;

// Four-byte rows: the blend loops read and write words where they can, and a
// stride that is not a multiple of the pixel size is the one thing they cannot
// assume.
static size_t aligned_stride(size_t value) { return (value + 3u) & ~(size_t)3u; }

static bool format_bytes(lv_color_format_t cf, int w, size_t *px_size, size_t *stride) {
	switch (cf) {
	case LV_COLOR_FORMAT_A8:
		*px_size = 1;
		break;
	case LV_COLOR_FORMAT_ARGB8888:
		*px_size = 4;
		break;
	default:
		return false;
	}
	*stride = aligned_stride((size_t)w * *px_size);
	return true;
}

const lv_image_dsc_t *iconscale_fit(const lv_image_dsc_t *src, int max_side) {
	if (!src || max_side <= 0) {
		return NULL;
	}

	int src_w = (int)src->header.w;
	int src_h = (int)src->header.h;
	int side = src_w > src_h ? src_w : src_h;
	if (src_w <= 0 || src_h <= 0 || side <= 0) {
		return NULL;
	}
	if (max_side >= side) {
		return src;
	}

	int w = (int)((int64_t)src_w * max_side / side);
	int h = (int)((int64_t)src_h * max_side / side);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	for (int i = 0; i < entry_count; i++) {
		if (entries[i].src == src && entries[i].w == (uint32_t)w && entries[i].h == (uint32_t)h) {
			return &entries[i].dsc;
		}
	}
	if (entry_count >= ICONSCALE_SLOTS) {
		return NULL;
	}

	size_t px_size = 0;
	size_t stride = 0;
	if (!format_bytes(src->header.cf, w, &px_size, &stride)) {
		return NULL;
	}

	size_t bytes = stride * (size_t)h;
	if (bytes_used + bytes > ICONSCALE_BYTES) {
		return NULL;
	}
	bool big = bytes > ICONSCALE_BIG_ENTRY;
	if (big && big_bytes_used + bytes > ICONSCALE_BIG_BYTES) {
		return NULL;
	}

	uint8_t *data = malloc(bytes);
	if (!data) {
		return NULL;
	}

	int src_stride = (int)src->header.stride;
	if (src_stride < src_w * (int)px_size) {
		src_stride = src_w * (int)px_size;
	}

	bool ok;
	if (src->header.cf == LV_COLOR_FORMAT_A8) {
		ok = resample_fpu_plane8(src->data, src_w, src_h, src_stride, data, w, h, (int)stride);
	} else {
		ok = resample_fpu_argb8888(src->data, src_w, src_h, src_stride, data, w, h, (int)stride);
	}
	if (!ok) {
		free(data);
		return NULL;
	}

	iconscale_entry_t *entry = &entries[entry_count++];
	memset(entry, 0, sizeof(*entry));
	entry->src = src;
	entry->w = (uint32_t)w;
	entry->h = (uint32_t)h;

	entry->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	entry->dsc.header.cf = src->header.cf;
	entry->dsc.header.w = (uint32_t)w;
	entry->dsc.header.h = (uint32_t)h;
	entry->dsc.header.stride = (uint16_t)stride;
	entry->dsc.data_size = (uint32_t)bytes;
	entry->dsc.data = data;

	bytes_used += bytes;
	if (big) {
		big_bytes_used += bytes;
	}
	return &entry->dsc;
}

void iconscale_set(lv_obj_t *icon, const lv_image_dsc_t *src, int max_side) {
	if (!icon || !src) {
		return;
	}

	const lv_image_dsc_t *scaled = iconscale_fit(src, max_side);
	if (scaled && scaled != src) {
		lv_image_set_src(icon, scaled);
		lv_image_set_scale(icon, LV_SCALE_NONE);
		return;
	}

	lv_image_set_src(icon, src);

	int side = (int)src->header.w > (int)src->header.h ? (int)src->header.w : (int)src->header.h;
	if (max_side > 0 && side > 0) {
		lv_image_set_scale(icon, (uint32_t)((int64_t)LV_SCALE_NONE * max_side / side));
	} else {
		lv_image_set_scale(icon, LV_SCALE_NONE);
	}
}
