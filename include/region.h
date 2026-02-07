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

#pragma once

struct pixman_region16;

/* Maximum normalized coordinate value (representing 1.0) */
#define NVNC_REGION_NORM_MAX 32767

void nvnc_region_scale(struct pixman_region16* dst, struct pixman_region16* src,
		double h_scale, double v_scale);
void nvnc_region_translate(struct pixman_region16* dst,
		struct pixman_region16* src, int x, int y);

/* Normalize region from pixel coordinates to [0, NVNC_REGION_NORM_MAX] */
void nvnc_region_normalize(struct pixman_region16* dst,
		const struct pixman_region16* src,
		uint32_t width, uint32_t height);

/* Denormalize region from [0, NVNC_REGION_NORM_MAX] to pixel coordinates */
void nvnc_region_denormalize(struct pixman_region16* dst,
		const struct pixman_region16* src,
		uint32_t width, uint32_t height);
