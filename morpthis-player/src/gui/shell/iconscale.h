#ifndef ICONSCALE_H
#define ICONSCALE_H

#include "lvgl/lvgl.h"

// Icons drawn at a size other than their own cost a transformed draw on every
// frame: LVGL resamples the bitmap into a scratch buffer allocated per draw,
// blends it, and frees the buffer, all inside the frame. A scrolling list that
// shows a dozen of them pays that a dozen times a frame, on the interface
// thread.
//
// A page knows its icon sizes when it is built, so the resampling can happen
// once, on the FPU, and the frame can afterwards draw the already-sized bitmap:
// no transform, no scratch buffer, no malloc per icon per frame.
//
// The cache lives for the process. A descriptor handed out is never freed or
// reused, because widgets keep the pointer. When the budget or the slot count
// runs out, iconscale_fit() answers NULL and the caller keeps the old
// lv_image_set_scale() path.
//
// Called from the interface thread only, while a page is being built.

// `src` scaled so its longer side is `max_side` pixels, or `src` itself when it
// is already that size. NULL when the format is not A8 or ARGB8888, or when the
// cache has no room left.
const lv_image_dsc_t *iconscale_fit(const lv_image_dsc_t *src, int max_side);

// Shows `src` in `icon` with its longer side at `max_side`, pre-scaled when
// possible; the widget's own box is left alone. On failure this is exactly the
// old lv_image_set_scale() call, so a page looks the same either way.
void iconscale_set(lv_obj_t *icon, const lv_image_dsc_t *src, int max_side);

#endif /* ICONSCALE_H */
