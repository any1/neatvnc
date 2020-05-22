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

#include "rfb-proto.h"
#include "pixels.h"
#include <stdlib.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

#define POPCOUNT(x) __builtin_popcount(x)

void pixel32_to_cpixel(uint8_t* restrict dst,
                       const struct rfb_pixel_format* dst_fmt,
                       const uint32_t* restrict src,
                       const struct rfb_pixel_format* src_fmt,
                       size_t bytes_per_cpixel, size_t len)
{
	assert(src_fmt->true_colour_flag);
	assert(src_fmt->bits_per_pixel == 32);
	assert(src_fmt->depth <= 32);
	assert(dst_fmt->true_colour_flag);
	assert(dst_fmt->bits_per_pixel <= 32);
	assert(dst_fmt->depth <= 24);
	assert(bytes_per_cpixel <= 4 && bytes_per_cpixel >= 1);

	if (src_fmt->bits_per_pixel == dst_fmt->bits_per_pixel &&
	    src_fmt->depth == dst_fmt->depth &&
	    src_fmt->big_endian_flag == dst_fmt->big_endian_flag &&
	    src_fmt->red_shift == dst_fmt->red_shift &&
	    src_fmt->red_max == dst_fmt->red_max &&
	    src_fmt->green_shift == dst_fmt->green_shift &&
	    src_fmt->green_max == dst_fmt->green_max &&
	    src_fmt->blue_shift == dst_fmt->blue_shift &&
	    src_fmt->blue_max == dst_fmt->blue_max)
	{
		memcpy(dst, src, len * bytes_per_cpixel);
		return;
	}

	uint32_t src_red_shift = src_fmt->red_shift;
	uint32_t src_green_shift = src_fmt->green_shift;
	uint32_t src_blue_shift = src_fmt->blue_shift;

	uint32_t dst_red_shift = dst_fmt->red_shift;
	uint32_t dst_green_shift = dst_fmt->green_shift;
	uint32_t dst_blue_shift = dst_fmt->blue_shift;

	uint32_t src_red_max = src_fmt->red_max;
	uint32_t src_green_max = src_fmt->green_max;
	uint32_t src_blue_max = src_fmt->blue_max;

	uint32_t src_red_bits = POPCOUNT(src_fmt->red_max);
	uint32_t src_green_bits = POPCOUNT(src_fmt->green_max);
	uint32_t src_blue_bits = POPCOUNT(src_fmt->blue_max);

	uint32_t dst_red_bits = POPCOUNT(dst_fmt->red_max);
	uint32_t dst_green_bits = POPCOUNT(dst_fmt->green_max);
	uint32_t dst_blue_bits = POPCOUNT(dst_fmt->blue_max);

	uint32_t dst_endian_correction;

#define CONVERT_PIXELS(cpx, px)                                                \
	{                                                                      \
		uint32_t r, g, b;                                              \
		r = ((px >> src_red_shift) & src_red_max) << dst_red_bits      \
		        >> src_red_bits << dst_red_shift;                      \
		g = ((px >> src_green_shift) & src_green_max) << dst_green_bits\
		        >> src_green_bits << dst_green_shift;                  \
		b = ((px >> src_blue_shift) & src_blue_max) << dst_blue_bits   \
		        >> src_blue_bits << dst_blue_shift;                    \
		cpx = r | g | b;                                               \
	}

	switch (bytes_per_cpixel) {
	case 4:
		if (dst_fmt->big_endian_flag) {
			while (len--) {
				uint32_t cpx, px = *src++;

				CONVERT_PIXELS(cpx, px)

				*dst++ = (cpx >> 24) & 0xff;
				*dst++ = (cpx >> 16) & 0xff;
				*dst++ = (cpx >> 8) & 0xff;
				*dst++ = (cpx >> 0) & 0xff;
			}
		} else {
			while (len--) {
				uint32_t cpx, px = *src++;

				CONVERT_PIXELS(cpx, px)

				*dst++ = (cpx >> 0) & 0xff;
				*dst++ = (cpx >> 8) & 0xff;
				*dst++ = (cpx >> 16) & 0xff;
				*dst++ = (cpx >> 24) & 0xff;
			}
		}
		break;
	case 3:
		if (dst_fmt->bits_per_pixel == 32 && dst_fmt->depth <= 24) {
			uint32_t min_dst_shift = dst_red_shift;
			if (min_dst_shift > dst_green_shift)
				min_dst_shift = dst_green_shift;
			if (min_dst_shift > dst_blue_shift)
				min_dst_shift = dst_blue_shift;

			dst_red_shift -= min_dst_shift;
			dst_green_shift -= min_dst_shift;
			dst_blue_shift -= min_dst_shift;
		}

		dst_endian_correction = dst_fmt->big_endian_flag ? 16 : 0;

		while (len--) {
			uint32_t cpx, px = *src++;

			CONVERT_PIXELS(cpx, px)

			*dst++ = (cpx >> (0 ^ dst_endian_correction)) & 0xff;
			*dst++ = (cpx >> 8) & 0xff;
			*dst++ = (cpx >> (16 ^ dst_endian_correction)) & 0xff;
		}
		break;
	case 2:
		dst_endian_correction = dst_fmt->big_endian_flag ? 8 : 0;

		while (len--) {
			uint32_t cpx, px = *src++;

			CONVERT_PIXELS(cpx, px)

			*dst++ = (cpx >> (0 ^ dst_endian_correction)) & 0xff;
			*dst++ = (cpx >> (8 ^ dst_endian_correction)) & 0xff;
		}
		break;
	case 1:
		while (len--) {
			uint32_t cpx, px = *src++;

			CONVERT_PIXELS(cpx, px)

			*dst++ = cpx & 0xff;
		}
		break;
	default:
		abort();
	}

#undef CONVERT_PIXELS
}

/* clang-format off */
int rfb_pixfmt_from_fourcc(struct rfb_pixel_format *dst, uint32_t src) {
	switch (src & ~DRM_FORMAT_BIG_ENDIAN) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		dst->red_shift = 24;
		dst->green_shift = 16;
		dst->blue_shift = 8;
		goto bpp_32;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		dst->red_shift = 8;
		dst->green_shift = 16;
		dst->blue_shift = 24;
		goto bpp_32;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		dst->red_shift = 16;
		dst->green_shift = 8;
		dst->blue_shift = 0;
		goto bpp_32;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		dst->red_shift = 0;
		dst->green_shift = 8;
		dst->blue_shift = 16;
bpp_32:
		dst->bits_per_pixel = 32;
		dst->depth = 24;
		dst->red_max = 0xff;
		dst->green_max = 0xff;
		dst->blue_max = 0xff;
		break;
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX4444:
		dst->red_shift = 12;
		dst->green_shift = 8;
		dst->blue_shift = 4;
		goto bpp_16;
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX4444:
		dst->red_shift = 4;
		dst->green_shift = 8;
		dst->blue_shift = 12;
		goto bpp_16;
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_XRGB4444:
		dst->red_shift = 8;
		dst->green_shift = 4;
		dst->blue_shift = 0;
		goto bpp_16;
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR4444:
		dst->red_shift = 0;
		dst->green_shift = 4;
		dst->blue_shift = 8;
bpp_16:
		dst->bits_per_pixel = 16;
		dst->depth = 12;
		dst->red_max = 0x7f;
		dst->green_max = 0x7f;
		dst->blue_max = 0x7f;
		break;
	default:
		return -1;
	}

	dst->big_endian_flag = !!(src & DRM_FORMAT_BIG_ENDIAN);
	dst->true_colour_flag = 1;

	return 0;
};
