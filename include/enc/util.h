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

#pragma once

#include "rfb-proto.h"

#include <stdint.h>

struct vec;
struct pixman_region16;

int nvnc__encode_rect_head(struct vec* dst, enum rfb_encodings encoding,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height);
uint32_t nvnc__calc_bytes_per_cpixel(const struct rfb_pixel_format* fmt);
uint32_t nvnc__calculate_region_area(struct pixman_region16* region);

struct encoded_frame* nvnc__encoded_frame_new(void* payload, size_t size,
		int n_rects, uint16_t width, uint16_t height, uint64_t pts);
