/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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

#include "neatvnc.h"
#include "rfb-proto.h"
#include "vec.h"
#include "fb.h"
#include "pixels.h"

#include <pixman.h>

int raw_encode_box(struct vec* dst, const struct rfb_pixel_format* dst_fmt,
                   const struct nvnc_fb* fb,
                   const struct rfb_pixel_format* src_fmt, int x_start,
                   int y_start, int stride, int width, int height)
{
	int rc = -1;

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_RAW),
		.x = htons(x_start),
		.y = htons(y_start),
		.width = htons(width),
		.height = htons(height),
	};

	rc = vec_append(dst, &rect, sizeof(rect));
	if (rc < 0)
		return -1;

	uint32_t* b = fb->addr;

	int bpp = dst_fmt->bits_per_pixel / 8;

	rc = vec_reserve(dst, width * height * bpp + dst->len);
	if (rc < 0)
		return -1;

	uint8_t* d = dst->data;

	for (int y = y_start; y < y_start + height; ++y) {
		pixel32_to_cpixel(d + dst->len, dst_fmt,
		                  b + x_start + y * stride, src_fmt,
		                  bpp, width);
		dst->len += width * bpp;
	}

	return 0;
}

int raw_encode_frame(struct vec* dst, const struct rfb_pixel_format* dst_fmt,
                     const struct nvnc_fb* src,
                     const struct rfb_pixel_format* src_fmt,
                     struct pixman_region16* region)
{
	int rc = -1;

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(region, &n_rects);
	if (n_rects > UINT16_MAX) {
		box = pixman_region_extents(region);
		n_rects = 1;
	}

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(n_rects),
	};

	rc = vec_reserve(dst, src->width * src->height * 4);
	if (rc < 0)
		return -1;

	rc = vec_append(dst, &head, sizeof(head));
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = raw_encode_box(dst, dst_fmt, src, src_fmt, x, y,
		                    src->width, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	return 0;
}
