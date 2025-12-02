/*
 * Copyright (c) 2020 - 2021 Andri Yngvason
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
 *
 * For code borrowed from wlroots:
 * Copyright (c) 2017, 2018 Drew DeVault
 * Copyright (c) 2014 Jari Vetoniemi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "transform-util.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <pixman.h>

/* Note: This function yields the inverse pixman transform of the
 * nvnc_transform.
 */
void nvnc_transform_to_pixman_transform(pixman_transform_t* dst,
		enum nvnc_transform src, int width, int height)
{
#define F1 pixman_fixed_1
	switch (src) {
	case NVNC_TRANSFORM_NORMAL:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_180:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_180:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	}
#undef F1

	abort();
}

bool nvnc_is_transform_90_degrees(enum nvnc_transform transform)
{
	switch (transform) {
	case NVNC_TRANSFORM_90:
	case NVNC_TRANSFORM_270:
	case NVNC_TRANSFORM_FLIPPED_90:
	case NVNC_TRANSFORM_FLIPPED_270:
		return true;
	default:
		break;
	}

	return false;
}

void nvnc_transform_dimensions(enum nvnc_transform transform, uint32_t* width,
		uint32_t* height)
{
	if (nvnc_is_transform_90_degrees(transform)) {
		uint32_t tmp = *width;
		*width = *height;
		*height = tmp;
	}
}

/* Borrowed this from wlroots */
void nvnc_transform_region(struct pixman_region16* dst,
		struct pixman_region16* src, enum nvnc_transform transform,
		int width, int height)
{
	if (transform == NVNC_TRANSFORM_NORMAL) {
		pixman_region_copy(dst, src);
		return;
	}

	int nrects = 0;
	pixman_box16_t* src_rects = pixman_region_rectangles(src, &nrects);

	pixman_box16_t* dst_rects = malloc(nrects * sizeof(*dst_rects));
	if (dst_rects == NULL) {
		return;
	}

	for (int i = 0; i < nrects; ++i) {
		switch (transform) {
		case NVNC_TRANSFORM_NORMAL:
			dst_rects[i].x1 = src_rects[i].x1;
			dst_rects[i].y1 = src_rects[i].y1;
			dst_rects[i].x2 = src_rects[i].x2;
			dst_rects[i].y2 = src_rects[i].y2;
			break;
		case NVNC_TRANSFORM_90:
			dst_rects[i].x1 = height - src_rects[i].y2;
			dst_rects[i].y1 = src_rects[i].x1;
			dst_rects[i].x2 = height - src_rects[i].y1;
			dst_rects[i].y2 = src_rects[i].x2;
			break;
		case NVNC_TRANSFORM_180:
			dst_rects[i].x1 = width - src_rects[i].x2;
			dst_rects[i].y1 = height - src_rects[i].y2;
			dst_rects[i].x2 = width - src_rects[i].x1;
			dst_rects[i].y2 = height - src_rects[i].y1;
			break;
		case NVNC_TRANSFORM_270:
			dst_rects[i].x1 = src_rects[i].y1;
			dst_rects[i].y1 = width - src_rects[i].x2;
			dst_rects[i].x2 = src_rects[i].y2;
			dst_rects[i].y2 = width - src_rects[i].x1;
			break;
		case NVNC_TRANSFORM_FLIPPED:
			dst_rects[i].x1 = width - src_rects[i].x2;
			dst_rects[i].y1 = src_rects[i].y1;
			dst_rects[i].x2 = width - src_rects[i].x1;
			dst_rects[i].y2 = src_rects[i].y2;
			break;
		case NVNC_TRANSFORM_FLIPPED_90:
			dst_rects[i].x1 = src_rects[i].y1;
			dst_rects[i].y1 = src_rects[i].x1;
			dst_rects[i].x2 = src_rects[i].y2;
			dst_rects[i].y2 = src_rects[i].x2;
			break;
		case NVNC_TRANSFORM_FLIPPED_180:
			dst_rects[i].x1 = src_rects[i].x1;
			dst_rects[i].y1 = height - src_rects[i].y2;
			dst_rects[i].x2 = src_rects[i].x2;
			dst_rects[i].y2 = height - src_rects[i].y1;
			break;
		case NVNC_TRANSFORM_FLIPPED_270:
			dst_rects[i].x1 = height - src_rects[i].y2;
			dst_rects[i].y1 = width - src_rects[i].x2;
			dst_rects[i].x2 = height - src_rects[i].y1;
			dst_rects[i].y2 = width - src_rects[i].x1;
			break;
		}
	}

	pixman_region_fini(dst);
	pixman_region_init_rects(dst, dst_rects, nrects);
	free(dst_rects);
}
