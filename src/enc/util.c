/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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

#include "enc/encoder.h"
#include "enc/util.h"
#include "rfb-proto.h"
#include "vec.h"

#include <stdlib.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <pixman.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

int encode_rect_head(struct vec* dst, enum rfb_encodings encoding,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct rfb_server_fb_rect head = {
		.encoding = htonl(encoding),
		.x = htons(x),
		.y = htons(y),
		.width = htons(width),
		.height = htons(height),
	};

	return vec_append(dst, &head, sizeof(head));
}

uint32_t calc_bytes_per_cpixel(const struct rfb_pixel_format* fmt)
{
	if (fmt->bits_per_pixel == 32) {
		/*
		 * CPIXEL size calculation for ZRLE/TRLE encodings.
		 *
		 * RFC 6143 Section 7.7.5 states cpixel is 3 bytes only when ALL
		 * of these conditions are met:
		 *   1. true-color-flag is non-zero
		 *   2. bits-per-pixel is 32
		 *   3. depth is 24 or less
		 *   4. all RGB bits fit in the least or most significant 3 bytes
		 *
		 * However, we use a practical approach adopted by other major VNC
		 * server implementations: check actual bit positions rather than
		 * the depth field. TigerVNC, libvncserver, and TurboVNC all use
		 * this approach - they check whether RGB values fit in 3 bytes
		 * by examining shifts and max values, ignoring depth entirely.
		 *
		 * This is necessary for macOS Screen Sharing compatibility.
		 * macOS sends bpp=32, depth=32, shifts=16,8,0 but expects
		 * 3-byte cpixels. There is no wire negotiation for cpixel size,
		 * so both sides must calculate it identically.
		 *
		 * This is a common approach among VNC implementations. If RGB
		 * fits in 3 bytes, we use 3 bytes regardless of depth.
		 */
		int max_shift = fmt->red_shift;
		if (fmt->green_shift > max_shift)
			max_shift = fmt->green_shift;
		if (fmt->blue_shift > max_shift)
			max_shift = fmt->blue_shift;

		int min_shift = fmt->red_shift;
		if (fmt->green_shift < min_shift)
			min_shift = fmt->green_shift;
		if (fmt->blue_shift < min_shift)
			min_shift = fmt->blue_shift;

		/* fitsInLS3Bytes: RGB in least significant 3 bytes */
		bool fits_in_ls3 = (max_shift <= 16);
		/* fitsInMS3Bytes: RGB in most significant 3 bytes */
		bool fits_in_ms3 = (min_shift >= 8);

		if (fits_in_ls3 || fits_in_ms3)
			return 3;
		return 4;
	}
	return UDIV_UP(fmt->bits_per_pixel, 8);
}

uint32_t calculate_region_area(struct pixman_region16* region)
{
	uint32_t area = 0;

	int n_rects = 0;
	struct pixman_box16* rects = pixman_region_rectangles(region,
			&n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int width = rects[i].x2 - rects[i].x1;
		int height = rects[i].y2 - rects[i].y1;
		area += width * height;
	}

	return area;
}

struct encoded_frame* encoded_frame_new(void* payload, size_t size, int n_rects,
		uint16_t width, uint16_t height, uint64_t pts)
{
	struct encoded_frame* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->buf.ref = 1;
	self->buf.size = size;
	self->buf.payload = payload;

	self->n_rects = n_rects;
	self->width = width;
	self->height = height;
	self->pts = pts;

	return self;
}
