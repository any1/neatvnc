/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
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

#include <stdint.h>
#include <unistd.h>
#include <pixman.h>
#include <stdbool.h>

struct rfb_pixel_format;
struct rfb_set_colour_map_entries_msg;

enum format_rating_flags {
	FORMAT_RATING_NEED_ALPHA = 1 << 0,
	FORMAT_RATING_PREFER_LINEAR = 1 << 1,
};

struct nvnc_pixel_format {
	uint32_t bytes_per_pixel;
	uint32_t red_shift;
	uint32_t green_shift;
	uint32_t blue_shift;
	uint32_t red_size;
	uint32_t green_size;
	uint32_t blue_size;
	uint32_t red_max;
	uint32_t green_max;
	uint32_t blue_max;
};

extern const uint32_t nvnc_supported_pixel_formats[];

void nvnc_convert_pixels(uint8_t* restrict dst,
		const struct nvnc_pixel_format* dst_fmt,
		const uint8_t* restrict src,
		const struct nvnc_pixel_format* src_fmt, size_t len);

int nvnc_pixel_format_from_fourcc(struct nvnc_pixel_format* dst, uint32_t src);
void nvnc_pixel_format_from_rfb(struct nvnc_pixel_format* dst,
		const struct rfb_pixel_format* src);
void nvnc_pixel_format_to_rfb(struct rfb_pixel_format* dst,
		const struct nvnc_pixel_format* src);
int nvnc_pixel_format_depth(const struct nvnc_pixel_format* fmt);
void nvnc_pixel_format_to_cpixel(struct nvnc_pixel_format* dst,
		const struct nvnc_pixel_format* src);

int nvnc__pixel_size_from_fourcc(uint32_t fourcc);

bool fourcc_to_pixman_fmt(pixman_format_code_t* dst, uint32_t src);

bool extract_alpha_mask(uint8_t* dst, const void* src, uint32_t format,
		size_t len);

const char* drm_format_to_string(uint32_t fmt);
const char* nvnc_pixel_format_to_string(const struct nvnc_pixel_format* fmt);
void make_rgb332_pal8_map(struct rfb_set_colour_map_entries_msg* msg);

double rate_pixel_format(uint32_t format, uint64_t modifier,
		enum format_rating_flags flags, int target_depth);
