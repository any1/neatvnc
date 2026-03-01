/*
 * Copyright (c) 2022 - 2026 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "cursor.h"

#include "fb.h"
#include "pixels.h"
#include "rfb-proto.h"
#include "vec.h"
#include "enc/util.h"
#include "compositor.h"
#include "transform-util.h"

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

int cursor_encode(struct vec* dst, struct rfb_pixel_format* pixfmt,
		struct nvnc_fb* image, uint32_t hotspot_x, uint32_t hotspot_y)
{
	int rc = -1;

	// Empty cursor
	if (!image)
		return nvnc__encode_rect_head(dst, RFB_ENCODING_CURSOR, 0, 0, 0, 0);

	if (nvnc_fb_map(image) < 0)
		return -1;

	struct nvnc_composite_fb cfb = {
		.n_fbs = 1,
		.fbs = { image },
	};

	uint32_t width = nvnc_composite_fb_width(&cfb);
	uint32_t height = nvnc_composite_fb_height(&cfb);
	struct nvnc_fb* fb = nvnc_fb_new(width, height, image->fourcc_format,
			width);
	assert(fb);

	composite_buffer_now(fb, &cfb, NULL);

	struct rfb_pixel_format srcfmt = { 0 };
	rc = rfb_pixfmt_from_fourcc(&srcfmt, fb->fourcc_format);
	if (rc < 0)
		goto failure;

	rc = nvnc__encode_rect_head(dst, RFB_ENCODING_CURSOR, hotspot_x, hotspot_y,
			width, height);
	if (rc < 0)
		goto failure;

	int bpp = pixfmt->bits_per_pixel / 8;
	size_t size = width * height;

	rc = vec_reserve(dst, dst->len + size * bpp + UDIV_UP(width, 8) * height);
	if (rc < 0)
		goto failure;

	uint8_t* dstdata = dst->data;
	dstdata += dst->len;

	pixel_to_cpixel(dstdata, pixfmt, fb->buffer->addr, &srcfmt, bpp, size);

	dst->len += size * bpp;
	dstdata = dst->data;
	dstdata += dst->len;

	for (uint32_t y = 0; y < height; ++y) {
		if (!extract_alpha_mask(dstdata + y * UDIV_UP(width, 8),
				(uint32_t*)fb->buffer->addr + y * fb->stride,
				fb->fourcc_format, width))
			goto failure;

		dst->len += UDIV_UP(width, 8);
	}

	rc = 0;
failure:
	nvnc_fb_unref(fb);
	return rc;
}
