/*
 * Copyright (c) 2022 - 2025 Andri Yngvason
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

#include <math.h>
#include <pixman.h>

#include "region.h"

void nvnc_region_scale(struct pixman_region16* dst, struct pixman_region16* src,
		double h_scale, double v_scale)
{
	if (h_scale == 1.0 && v_scale == 1.0) {
		pixman_region_copy(dst, src);
		return;
	}

	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = floor((double)r->x1 * h_scale);
		int x2 = ceil((double)r->x2 * h_scale);
		int y1 = floor((double)r->y1 * v_scale);
		int y2 = ceil((double)r->y2 * v_scale);

		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

void nvnc_region_translate(struct pixman_region16* dst,
		struct pixman_region16* src, int x, int y)
{
	if (x == 0 && y == 0) {
		pixman_region_copy(dst, src);
		return;
	}
	
	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = r->x1 + x;
		int x2 = r->x2 + x;
		int y1 = r->y1 + y;
		int y2 = r->y2 + y;

		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

void nvnc_region_normalize(struct pixman_region16* dst,
		const struct pixman_region16* src,
		uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0) {
		pixman_region_init(dst);
		return;
	}

	double h_scale = (double)NVNC_REGION_NORM_MAX / width;
	double v_scale = (double)NVNC_REGION_NORM_MAX / height;

	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(
			(struct pixman_region16*)src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = floor((double)r->x1 * h_scale);
		int x2 = ceil((double)r->x2 * h_scale);
		int y1 = floor((double)r->y1 * v_scale);
		int y2 = ceil((double)r->y2 * v_scale);

		// Clamp to valid range
		x1 = (x1 < 0) ? 0 : ((x1 > NVNC_REGION_NORM_MAX) ? NVNC_REGION_NORM_MAX : x1);
		x2 = (x2 < 0) ? 0 : ((x2 > NVNC_REGION_NORM_MAX) ? NVNC_REGION_NORM_MAX : x2);
		y1 = (y1 < 0) ? 0 : ((y1 > NVNC_REGION_NORM_MAX) ? NVNC_REGION_NORM_MAX : y1);
		y2 = (y2 < 0) ? 0 : ((y2 > NVNC_REGION_NORM_MAX) ? NVNC_REGION_NORM_MAX : y2);

		if (x2 > x1 && y2 > y1)
			pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

void nvnc_region_denormalize(struct pixman_region16* dst,
		const struct pixman_region16* src,
		uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0) {
		pixman_region_init(dst);
		return;
	}

	double h_scale = (double)width / NVNC_REGION_NORM_MAX;
	double v_scale = (double)height / NVNC_REGION_NORM_MAX;

	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(
			(struct pixman_region16*)src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = floor((double)r->x1 * h_scale);
		int x2 = ceil((double)r->x2 * h_scale);
		int y1 = floor((double)r->y1 * v_scale);
		int y2 = ceil((double)r->y2 * v_scale);

		// Clamp to valid buffer dimensions
		x1 = (x1 < 0) ? 0 : ((x1 > (int)width) ? (int)width : x1);
		x2 = (x2 < 0) ? 0 : ((x2 > (int)width) ? (int)width : x2);
		y1 = (y1 < 0) ? 0 : ((y1 > (int)height) ? (int)height : y1);
		y2 = (y2 < 0) ? 0 : ((y2 > (int)height) ? (int)height : y2);

		if (x2 > x1 && y2 > y1)
			pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}
