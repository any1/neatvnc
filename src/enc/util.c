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
	return fmt->bits_per_pixel == 32 ? UDIV_UP(fmt->depth, 8)
	                                 : UDIV_UP(fmt->bits_per_pixel, 8);
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
