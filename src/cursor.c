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

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

int cursor_encode(struct vec* dst, struct rfb_pixel_format* pixfmt,
		struct nvnc_fb* image, int x_hotspot, int y_hotspot)
{
	// TODO: Handle rotated cursors

	int rc = -1;

	struct rfb_pixel_format srcfmt = { 0 };
	rc = rfb_pixfmt_from_fourcc(&srcfmt, image->fourcc_format);
	if (rc < 0)
		return -1;

	rc = encode_rect_head(dst, RFB_ENCODING_CURSOR, x_hotspot, y_hotspot,
			image->width, image->height);
	if (rc < 0)
		return -1;

	int bpp = pixfmt->bits_per_pixel / 8;
	size_t size = image->width * image->height;

	rc = vec_reserve(dst, dst->len + size * bpp + UDIV_UP(size, 8));
	if (rc < 0)
		return -1;

	uint8_t* dstdata = dst->data;
	dstdata += dst->len;

	if(image->width == image->stride) {
		pixel32_to_cpixel(dstdata, pixfmt, image->addr, &srcfmt, bpp, size);
	} else {
		for (int y = 0; y < image->height; ++y) {
			pixel32_to_cpixel(dstdata + y * bpp * image->width, pixfmt,
					(uint32_t*)image->addr + y * image->stride,
					&srcfmt, bpp, size);
		}
	}

	dst->len += size * bpp;
	dstdata = dst->data;
	dstdata += dst->len;

	if(image->width == image->stride) {
		if (!extract_alpha_mask(dstdata, image->addr,
					image->fourcc_format, size))
			return -1;
	} else {
		// TODO
		abort();
	}

	dst->len += UDIV_UP(size, 8);
	return 0;
}
