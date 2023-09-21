/*
 * Copyright (c) 2022 Andri Yngvason
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
#include "enc-util.h"
#include "resampler.h"
#include "transform-util.h"

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

static struct nvnc_fb* apply_transform(struct nvnc_fb* fb)
{
	if (fb->transform == NVNC_TRANSFORM_NORMAL) {
		nvnc_fb_ref(fb);
		return fb;
	}

	uint32_t width = fb->width;
	uint32_t height = fb->height;

	nvnc_transform_dimensions(fb->transform, &width, &height);
	struct nvnc_fb* dst = nvnc_fb_new(width, height, fb->fourcc_format,
			width);
	assert(dst);

	// TODO: Don't assume bpp
	memset(dst->addr, 0, width * height * 4);

	resample_now(dst, fb, NULL);

	return dst;
}

int cursor_encode(struct vec* dst, struct rfb_pixel_format* pixfmt,
		struct nvnc_fb* image, uint32_t width, uint32_t height,
		uint32_t hotspot_x, uint32_t hotspot_y)
{
	int rc = -1;

	// Empty cursor
	if (!image)
		return encode_rect_head(dst, RFB_ENCODING_CURSOR, 0, 0, 0, 0);

	nvnc_transform_dimensions(image->transform, &width, &height);
	nvnc_transform_dimensions(image->transform, &hotspot_x, &hotspot_y);

	if (nvnc_fb_map(image) < 0)
		goto failure;

	image = apply_transform(image);

	assert(width <= image->width);
	assert(height <= image->height);

	struct rfb_pixel_format srcfmt = { 0 };
	rc = rfb_pixfmt_from_fourcc(&srcfmt, image->fourcc_format);
	if (rc < 0)
		goto failure;

	rc = encode_rect_head(dst, RFB_ENCODING_CURSOR, hotspot_x, hotspot_y,
			width, height);
	if (rc < 0)
		goto failure;

	int bpp = pixfmt->bits_per_pixel / 8;
	size_t size = width * height;

	rc = vec_reserve(dst, dst->len + size * bpp + UDIV_UP(size, 8));
	if (rc < 0)
		goto failure;

	uint8_t* dstdata = dst->data;
	dstdata += dst->len;

	int32_t src_byte_stride = image->stride * (srcfmt.bits_per_pixel / 8);

	if((int32_t)width == image->stride) {
		pixel_to_cpixel(dstdata, pixfmt, image->addr, &srcfmt, bpp, size);
	} else {
		for (uint32_t y = 0; y < height; ++y) {
			pixel_to_cpixel(dstdata + y * bpp * width, pixfmt,
					(uint8_t*)image->addr + y * src_byte_stride,
					&srcfmt, bpp, width);
		}
	}

	dst->len += size * bpp;
	dstdata = dst->data;
	dstdata += dst->len;

	for (uint32_t y = 0; y < height; ++y) {
		if (!extract_alpha_mask(dstdata + y * UDIV_UP(width, 8),
					(uint32_t*)image->addr + y * image->stride,
					image->fourcc_format, width))
			goto failure;

		dst->len += UDIV_UP(width, 8);
	}

	rc = 0;
failure:
	nvnc_fb_unref(image);
	return rc;
}
