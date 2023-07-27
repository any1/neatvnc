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

#include "rfb-proto.h"
#include "pixels.h"
#include <stdlib.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

#define POPCOUNT(x) __builtin_popcount(x)
#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define XSTR(s) STR(s)
#define STR(s) #s

static void pixel32_to_cpixel(uint8_t* restrict dst,
		const struct rfb_pixel_format* dst_fmt,
		const uint32_t* restrict src,
		const struct rfb_pixel_format* src_fmt,
		size_t bytes_per_cpixel, size_t len)
{
	assert(src_fmt->true_colour_flag);
	assert(src_fmt->depth <= 32);
	assert(dst_fmt->true_colour_flag);
	assert(dst_fmt->bits_per_pixel <= 32);
	assert(dst_fmt->depth <= 32);
	assert(bytes_per_cpixel <= 4 && bytes_per_cpixel >= 1);

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

void pixel_to_cpixel(uint8_t* restrict dst,
		const struct rfb_pixel_format* dst_fmt,
		const uint8_t* restrict src,
		const struct rfb_pixel_format* src_fmt,
		size_t bytes_per_cpixel, size_t len)
{
	if (src_fmt->bits_per_pixel == 32) {
		pixel32_to_cpixel(dst, dst_fmt, (uint32_t*)src, src_fmt, bytes_per_cpixel, len);
		return;
	}

	assert(src_fmt->true_colour_flag);
	assert(src_fmt->depth <= 32);
	assert(dst_fmt->true_colour_flag);
	assert(dst_fmt->bits_per_pixel <= 32);
	assert(dst_fmt->depth <= 32);
	assert(bytes_per_cpixel <= 4 && bytes_per_cpixel >= 1);

	uint32_t src_bpp = src_fmt->bits_per_pixel / 8;
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
				uint32_t cpx, px = 0;
				memcpy(&px, src, src_bpp);
				src += src_bpp;

				CONVERT_PIXELS(cpx, px)

				*dst++ = (cpx >> 24) & 0xff;
				*dst++ = (cpx >> 16) & 0xff;
				*dst++ = (cpx >> 8) & 0xff;
				*dst++ = (cpx >> 0) & 0xff;
			}
		} else {
			while (len--) {
				uint32_t cpx, px = 0;
				memcpy(&px, src, src_bpp);
				src += src_bpp;

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
			uint32_t cpx, px = 0;
			memcpy(&px, src, src_bpp);
			src += src_bpp;

			CONVERT_PIXELS(cpx, px)

			*dst++ = (cpx >> (0 ^ dst_endian_correction)) & 0xff;
			*dst++ = (cpx >> 8) & 0xff;
			*dst++ = (cpx >> (16 ^ dst_endian_correction)) & 0xff;
		}
		break;
	case 2:
		dst_endian_correction = dst_fmt->big_endian_flag ? 8 : 0;

		while (len--) {
			uint32_t cpx, px = 0;
			memcpy(&px, src, src_bpp);
			src += src_bpp;

			CONVERT_PIXELS(cpx, px)

			*dst++ = (cpx >> (0 ^ dst_endian_correction)) & 0xff;
			*dst++ = (cpx >> (8 ^ dst_endian_correction)) & 0xff;
		}
		break;
	case 1:
		while (len--) {
			uint32_t cpx, px = 0;
			memcpy(&px, src, src_bpp);
			src += src_bpp;

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
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
		dst->red_shift = 22;
		dst->green_shift = 12;
		dst->blue_shift = 2;
		goto bpp_32_10bit;
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
		dst->red_shift = 2;
		dst->green_shift = 12;
		dst->blue_shift = 22;
		goto bpp_32_10bit;
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XRGB2101010:
		dst->red_shift = 20;
		dst->green_shift = 10;
		dst->blue_shift = 0;
		goto bpp_32_10bit;
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
		dst->red_shift = 0;
		dst->green_shift = 10;
		dst->blue_shift = 20;
bpp_32_10bit:
		dst->bits_per_pixel = 32;
		dst->depth = 30;
		dst->red_max = 0x3ff;
		dst->green_max = 0x3ff;
		dst->blue_max = 0x3ff;
		break;
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
	case DRM_FORMAT_BGR888:
		dst->red_shift = 0;
		dst->green_shift = 8;
		dst->blue_shift = 16;
		goto bpp_24;
	case DRM_FORMAT_RGB888:
		dst->red_shift = 16;
		dst->green_shift = 8;
		dst->blue_shift = 0;
bpp_24:
		dst->bits_per_pixel = 24;
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
}

int pixel_size_from_fourcc(uint32_t fourcc)
{
	switch (fourcc & ~DRM_FORMAT_BIG_ENDIAN) {
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return 4;
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
		return 3;
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR4444:
		return 2;
	}

	return 0;
}

bool fourcc_to_pixman_fmt(pixman_format_code_t* dst, uint32_t src)
{
	assert(!(src & DRM_FORMAT_BIG_ENDIAN));

#define LOWER_R r
#define LOWER_G g
#define LOWER_B b
#define LOWER_A a
#define LOWER_X x
#define LOWER_
#define LOWER(x) LOWER_##x

#define CONCAT_(a, b) a ## b
#define CONCAT(a, b) CONCAT_(a, b)

#define FMT_DRM(x, y, z, v, a, b, c, d) DRM_FORMAT_##x##y##z##v##a##b##c##d

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(x)), a), LOWER(y)), b), LOWER(z)), c), LOWER(v)), d)
#else
#define FMT_PIXMAN(x, y, z, v, a, b, c, d) \
	CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(CONCAT(\
	PIXMAN_, LOWER(v)), d), LOWER(z)), c), LOWER(y)), b), LOWER(x)), a)
#endif

	switch (src) {
#define X(...) \
	case FMT_DRM(__VA_ARGS__): *dst = FMT_PIXMAN(__VA_ARGS__); break

	/* 32 bits */
	X(A,R,G,B,8,8,8,8);
	X(A,B,G,R,8,8,8,8);
	X(X,R,G,B,8,8,8,8);
	X(X,B,G,R,8,8,8,8);
	X(R,G,B,A,8,8,8,8);
	X(B,G,R,A,8,8,8,8);
	X(R,G,B,X,8,8,8,8);
	X(B,G,R,X,8,8,8,8);

	/* 24 bits */
	X(R,G,B,,8,8,8,);
	X(B,G,R,,8,8,8,);

	/* 16 bits */
	X(R,G,B,,5,6,5,);
	X(B,G,R,,5,6,5,);

	/* These are incompatible on big endian */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	X(A,R,G,B,2,10,10,10);
	X(X,R,G,B,2,10,10,10);
	X(A,B,G,R,2,10,10,10);
	X(X,B,G,R,2,10,10,10);
	X(A,R,G,B,1,5,5,5);
	X(A,B,G,R,1,5,5,5);
	X(X,R,G,B,1,5,5,5);
	X(X,B,G,R,1,5,5,5);
	X(A,R,G,B,4,4,4,4);
	X(A,B,G,R,4,4,4,4);
	X(X,R,G,B,4,4,4,4);
	X(X,B,G,R,4,4,4,4);
#endif

#undef X

	default: return false;
	}

	return true;
}

static bool extract_alpha_mask_rgba32(uint8_t* dst, const uint32_t* src,
		size_t len, int alpha_shift, uint32_t alpha_max)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t alpha = (src[i] >> alpha_shift) & alpha_max;
		uint8_t binary = !!(alpha > alpha_max / 2);
		dst[i / 8] |= binary << (7 - (i % 8));
	}

	return true;
}

static bool extract_alpha_mask_rgba16(uint8_t* dst, const uint16_t* src,
		size_t len, int alpha_shift)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t alpha = (src[i] >> alpha_shift) & 0xf;
		uint8_t binary = !!(alpha > 0xf / 2);
		dst[i / 8] |= binary << (7 - (i % 8));
	}

	return true;
}

// Note: The destination buffer must be at least UDIV_UP(len, 8) long.
bool extract_alpha_mask(uint8_t* dst, const void* src, uint32_t format,
		size_t len)
{
	memset(dst, 0, UDIV_UP(len, 8));

	switch (format & ~DRM_FORMAT_BIG_ENDIAN) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_BGRA1010102:
		return extract_alpha_mask_rgba32(dst, src, len, 0, 3);
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
		return extract_alpha_mask_rgba32(dst, src, len, 30, 3);
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
		return extract_alpha_mask_rgba32(dst, src, len, 0, 0xff);
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
		return extract_alpha_mask_rgba32(dst, src, len, 24, 0xff);
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_BGRA4444:
		return extract_alpha_mask_rgba16(dst, src, len, 0);
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_ABGR4444:
		return extract_alpha_mask_rgba16(dst, src, len, 12);
#else
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_BGRA1010102:
		return extract_alpha_mask_rgba32(dst, src, len, 30, 3);
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
		return extract_alpha_mask_rgba32(dst, src, len, 0, 3);
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
		return extract_alpha_mask_rgba32(dst, src, len, 24, 0xff);
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
		return extract_alpha_mask_rgba32(dst, src, len, 0, 0xff);
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_BGRA4444:
		return extract_alpha_mask_rgba16(dst, src, len, 12);
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_ABGR4444:
		return extract_alpha_mask_rgba16(dst, src, len, 0);
#endif
	}

	return false;
}

const char* drm_format_to_string(uint32_t fmt)
{
	switch (fmt) {
#define X(x) case DRM_FORMAT_ ## x: return XSTR(x);
	X(RGBA1010102) \
	X(RGBX1010102) \
	X(BGRA1010102) \
	X(BGRX1010102) \
	X(ARGB2101010) \
	X(XRGB2101010) \
	X(ABGR2101010) \
	X(XBGR2101010) \
	X(RGBA8888) \
	X(RGBX8888) \
	X(BGRA8888) \
	X(BGRX8888) \
	X(ARGB8888) \
	X(XRGB8888) \
	X(ABGR8888) \
	X(XBGR8888) \
	X(RGB888) \
	X(BGR888) \
	X(RGBA4444) \
	X(RGBX4444) \
	X(BGRA4444) \
	X(BGRX4444) \
	X(ARGB4444) \
	X(XRGB4444) \
	X(ABGR4444) \
	X(XBGR4444) \
	X(RGB565)
#undef X
	}
	return "UNKNOWN";
}

// Not exact, but close enough for debugging
const char* rfb_pixfmt_to_string(const struct rfb_pixel_format* fmt)
{
	if (!(fmt->red_max == fmt->green_max && fmt->red_max == fmt->blue_max))
		goto failure;

	uint32_t profile = (fmt->red_shift << 16) | (fmt->green_shift << 8)
		| (fmt->blue_shift);

	switch (profile) {
#define CASE(r, g, b) case ((r << 16) | (g << 8) | b)
	CASE(22, 10, 2): return "RGBX1010102";
	CASE(2, 12, 22): return "BGRX1010102";
	CASE(20, 10, 0): return "XRGB2101010";
	CASE(0, 10, 20): return "XBGR2101010";
	CASE(24, 16, 8): return "RGBX8888";
	CASE(8, 16, 24): return "BGRX8888";
	CASE(16, 8, 0):  return "XRGB8888";
	CASE(0, 8, 16):  return "XBGR8888";
	CASE(12, 8, 4):  return "RGBX4444";
	CASE(4, 8, 12):  return "BGRX4444";
	CASE(8, 4, 0):   return "XRGB4444";
	CASE(0, 4, 8):   return "XBGR4444";
	CASE(11, 5, 0):  return "RGB565";
#undef CASE
	}

failure:
	return "UNKNOWN";
}
