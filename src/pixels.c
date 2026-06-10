/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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
#include <string.h>
#include <libdrm/drm_fourcc.h>
#include <math.h>

#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NEVER_INLINE __attribute__((__noinline__))

#define POPCOUNT(x) __builtin_popcount(x)
#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define XSTR(s) STR(s)
#define STR(s) #s

#ifndef fourcc_mod_get_vendor
#define fourcc_mod_get_vendor(modifier) \
	(((modifier) >> 56) & 0xff)
#endif

#ifndef fourcc_mod_is_vendor
#define fourcc_mod_is_vendor(modifier, vendor) \
	(fourcc_mod_get_vendor(modifier) == DRM_FORMAT_MOD_VENDOR_## vendor)
#endif

#define PIXEL_FORMAT_TABLE \
	X(R, G, B, X, 10, 10, 10, 2) \
	A(R, G, B, A, 10, 10, 10, 2) \
	X(B, G, R, X, 10, 10, 10, 2) \
	A(B, G, R, A, 10, 10, 10, 2) \
	X(X, R, G, B, 2, 10, 10, 10) \
	A(A, R, G, B, 2, 10, 10, 10) \
	X(X, B, G, R, 2, 10, 10, 10) \
	A(A, B, G, R, 2, 10, 10, 10) \
	X(R, G, B, X, 8, 8, 8, 8) \
	A(R, G, B, A, 8, 8, 8, 8) \
	X(B, G, R, X, 8, 8, 8, 8) \
	A(B, G, R, A, 8, 8, 8, 8) \
	X(X, R, G, B, 8, 8, 8, 8) \
	A(A, R, G, B, 8, 8, 8, 8) \
	X(X, B, G, R, 8, 8, 8, 8) \
	A(A, B, G, R, 8, 8, 8, 8) \
	N(R, G, B, N, 8, 8, 8, 0) \
	N(B, G, R, N, 8, 8, 8, 0) \
	X(R, G, B, X, 4, 4, 4, 4) \
	A(R, G, B, A, 4, 4, 4, 4) \
	X(B, G, R, X, 4, 4, 4, 4) \
	A(B, G, R, A, 4, 4, 4, 4) \
	X(X, R, G, B, 4, 4, 4, 4) \
	A(A, R, G, B, 4, 4, 4, 4) \
	X(X, B, G, R, 4, 4, 4, 4) \
	A(A, B, G, R, 4, 4, 4, 4) \
	X(R, G, B, X, 5, 5, 5, 1) \
	A(R, G, B, A, 5, 5, 5, 1) \
	X(B, G, R, X, 5, 5, 5, 1) \
	A(B, G, R, A, 5, 5, 5, 1) \
	X(X, R, G, B, 1, 5, 5, 5) \
	A(A, R, G, B, 1, 5, 5, 5) \
	X(X, B, G, R, 1, 5, 5, 5) \
	A(A, B, G, R, 1, 5, 5, 5) \
	N(R, G, B, N, 5, 6, 5, 0) \
	N(B, G, R, N, 5, 6, 5, 0) \
	N(R, G, B, N, 3, 3, 2, 0) \
	N(B, G, R, N, 2, 3, 3, 0) \

#define FIRST_SHIFT_FROM_SIZES(a, b, c, d) ((b) + (c) + (d))
#define SECOND_SHIFT_FROM_SIZES(a, b, c, d) ((c) + (d))
#define THIRD_SHIFT_FROM_SIZES(a, b, c, d) (d)
#define FOURTH_SHIFT_FROM_SIZES(a, b, c, d) 0

#define BPP_FROM_SIZES(a, b, c, d) ((a) + (b) + (c) + (d))

#define IS_R_R 1
#define IS_R_G 0
#define IS_R_B 0
#define IS_R_A 0
#define IS_R_X 0
#define IS_R_N 0
#define IS_R(c) IS_R_##c

#define IS_G_R 0
#define IS_G_G 1
#define IS_G_B 0
#define IS_G_A 0
#define IS_G_X 0
#define IS_G_N 0
#define IS_G(c) IS_G_##c

#define IS_B_R 0
#define IS_B_G 0
#define IS_B_B 1
#define IS_B_A 0
#define IS_B_X 0
#define IS_B_N 0
#define IS_B(c) IS_B_##c

#define IS_A_R 0
#define IS_A_G 0
#define IS_A_B 0
#define IS_A_A 1
#define IS_A_X 0
#define IS_A_N 0
#define IS_A(c) IS_A_##c

#define RED_SHIFT(c1, c2, c3, c4, a, b, c, d) \
	(IS_R(c1) * FIRST_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_R(c2) * SECOND_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_R(c3) * THIRD_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_R(c4) * FOURTH_SHIFT_FROM_SIZES(a, b, c, d))

#define GREEN_SHIFT(c1, c2, c3, c4, a, b, c, d) \
	(IS_G(c1) * FIRST_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_G(c2) * SECOND_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_G(c3) * THIRD_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_G(c4) * FOURTH_SHIFT_FROM_SIZES(a, b, c, d))

#define BLUE_SHIFT(c1, c2, c3, c4, a, b, c, d) \
	(IS_B(c1) * FIRST_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_B(c2) * SECOND_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_B(c3) * THIRD_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_B(c4) * FOURTH_SHIFT_FROM_SIZES(a, b, c, d))

#define ALPHA_SHIFT(c1, c2, c3, c4, a, b, c, d) \
	(IS_A(c1) * FIRST_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_A(c2) * SECOND_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_A(c3) * THIRD_SHIFT_FROM_SIZES(a, b, c, d) \
	+ IS_A(c4) * FOURTH_SHIFT_FROM_SIZES(a, b, c, d))

#define RED_SIZE(c1, c2, c3, c4, a, b, c, d) \
	(IS_R(c1) * (a) + IS_R(c2) * (b) + IS_R(c3) * (c) + IS_R(c4) * (d))

#define GREEN_SIZE(c1, c2, c3, c4, a, b, c, d) \
	(IS_G(c1) * (a) + IS_G(c2) * (b) + IS_G(c3) * (c) + IS_G(c4) * (d))

#define BLUE_SIZE(c1, c2, c3, c4, a, b, c, d) \
	(IS_B(c1) * (a) + IS_B(c2) * (b) + IS_B(c3) * (c) + IS_B(c4) * (d))

#define ALPHA_SIZE(c1, c2, c3, c4, a, b, c, d) \
	(IS_A(c1) * (a) + IS_A(c2) * (b) + IS_A(c3) * (c) + IS_A(c4) * (d))

#define RGB_DEPTH(c1, c2, c3, c4, a, b, c, d) \
	(RED_SIZE(c1, c2, c3, c4, a, b, c, d) \
	+ GREEN_SIZE(c1, c2, c3, c4, a, b, c, d) \
	+ BLUE_SIZE(c1, c2, c3, c4, a, b, c, d))

struct nvnc_format_conversion_recipe {
	const struct nvnc_pixel_format* restrict src;
	const struct nvnc_pixel_format* restrict dst;
};

const uint32_t nvnc_supported_pixel_formats[] = {
#define X(c1, c2, c3, c4, a, b, c, d) DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d,
#define A X
#define N(c1, c2, c3, c4, a, b, c, d) DRM_FORMAT_##c1##c2##c3##a##b##c,
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
	DRM_FORMAT_INVALID
};

static ALWAYS_INLINE uint32_t convert_pixel(uint32_t px,
		const struct nvnc_format_conversion_recipe* recipe)
{
	const struct nvnc_pixel_format* s = recipe->src;
	const struct nvnc_pixel_format* d = recipe->dst;

	uint32_t r = ((px >> s->red_shift) & s->red_max)
		<< d->red_size >> s->red_size << d->red_shift;
	uint32_t g = ((px >> s->green_shift) & s->green_max)
		<< d->green_size >> s->green_size << d->green_shift;
	uint32_t b = ((px >> s->blue_shift) & s->blue_max)
		<< d->blue_size >> s->blue_size << d->blue_shift;
	return r | g | b;
}

static ALWAYS_INLINE void convert_pixels_dst4_src(uint32_t src_bpp,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len--) {
		uint32_t px = 0;
		memcpy(&px, src, src_bpp);
		src += src_bpp;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = (cpx >> 0) & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
		*dst++ = (cpx >> 16) & 0xff;
		*dst++ = (cpx >> 24) & 0xff;
	}
}

static ALWAYS_INLINE void convert_pixels_dst4_src3(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len-- > 1) {
		uint32_t px = 0;
		memcpy(&px, src, 4);
		src += 3;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = (cpx >> 0) & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
		*dst++ = (cpx >> 16) & 0xff;
		*dst++ = (cpx >> 24) & 0xff;
	}

	uint32_t px = 0;
	memcpy(&px, src, 3);

	uint32_t cpx = convert_pixel(px, recipe);

	*dst++ = (cpx >> 0) & 0xff;
	*dst++ = (cpx >> 8) & 0xff;
	*dst++ = (cpx >> 16) & 0xff;
	*dst++ = (cpx >> 24) & 0xff;
}

static NEVER_INLINE void convert_pixels_dst4(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	switch (recipe->src->bytes_per_pixel) {
	case 4: convert_pixels_dst4_src(4, dst, src, len, recipe); break;
	case 3: convert_pixels_dst4_src3(dst, src, len, recipe); break;
	case 2: convert_pixels_dst4_src(2, dst, src, len, recipe); break;
	case 1: convert_pixels_dst4_src(1, dst, src, len, recipe); break;
	default: abort();
	}
}

static ALWAYS_INLINE void convert_pixels_dst3_src(uint32_t src_bpp,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len--) {
		uint32_t px = 0;
		memcpy(&px, src, src_bpp);
		src += src_bpp;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = cpx & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
		*dst++ = (cpx >> 16) & 0xff;
	}
}

static ALWAYS_INLINE void convert_pixels_dst3_src3(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len-- > 1) {
		uint32_t px = 0;
		memcpy(&px, src, 4);
		src += 3;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = (cpx >> 0) & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
		*dst++ = (cpx >> 16) & 0xff;
	}

	uint32_t px = 0;
	memcpy(&px, src, 3);

	uint32_t cpx = convert_pixel(px, recipe);

	*dst++ = (cpx >> 0) & 0xff;
	*dst++ = (cpx >> 8) & 0xff;
	*dst++ = (cpx >> 16) & 0xff;
}

static NEVER_INLINE void convert_pixels_dst3(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	switch (recipe->src->bytes_per_pixel) {
	case 4: convert_pixels_dst3_src(4, dst, src, len, recipe); break;
	case 3: convert_pixels_dst3_src3(dst, src, len, recipe); break;
	case 2: convert_pixels_dst3_src(2, dst, src, len, recipe); break;
	case 1: convert_pixels_dst3_src(1, dst, src, len, recipe); break;
	default: abort();
	}
}

static ALWAYS_INLINE void convert_pixels_dst2_src(uint32_t src_bpp,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len--) {
		uint32_t px = 0;
		memcpy(&px, src, src_bpp);
		src += src_bpp;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = cpx & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
	}
}

static ALWAYS_INLINE void convert_pixels_dst2_src3(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len-- > 1) {
		uint32_t px = 0;
		memcpy(&px, src, 4);
		src += 3;

		uint32_t cpx = convert_pixel(px, recipe);

		*dst++ = (cpx >> 0) & 0xff;
		*dst++ = (cpx >> 8) & 0xff;
	}

	uint32_t px = 0;
	memcpy(&px, src, 3);

	uint32_t cpx = convert_pixel(px, recipe);

	*dst++ = (cpx >> 0) & 0xff;
	*dst++ = (cpx >> 8) & 0xff;
}

static NEVER_INLINE void convert_pixels_dst2(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	switch (recipe->src->bytes_per_pixel) {
	case 4: convert_pixels_dst2_src(4, dst, src, len, recipe); break;
	case 3: convert_pixels_dst2_src3(dst, src, len, recipe); break;
	case 2: convert_pixels_dst2_src(2, dst, src, len, recipe); break;
	case 1: convert_pixels_dst2_src(1, dst, src, len, recipe); break;
	default: abort();
	}
}

static ALWAYS_INLINE void convert_pixels_dst1_src(uint32_t src_bpp,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len--) {
		uint32_t px = 0;
		memcpy(&px, src, src_bpp);
		src += src_bpp;

		*dst++ = convert_pixel(px, recipe) & 0xff;
	}
}

static ALWAYS_INLINE void convert_pixels_dst1_src3(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	while (len-- > 1) {
		uint32_t px = 0;
		memcpy(&px, src, 4);
		src += 3;

		*dst++ = convert_pixel(px, recipe) & 0xff;
	}

	uint32_t px = 0;
	memcpy(&px, src, 3);

	*dst++ = convert_pixel(px, recipe) & 0xff;
}

static NEVER_INLINE void convert_pixels_dst1(uint8_t* restrict dst,
		const uint8_t* restrict src, size_t len,
		const struct nvnc_format_conversion_recipe* recipe)
{
	switch (recipe->src->bytes_per_pixel) {
	case 4: convert_pixels_dst1_src(4, dst, src, len, recipe); break;
	case 3: convert_pixels_dst1_src3(dst, src, len, recipe); break;
	case 2: convert_pixels_dst1_src(2, dst, src, len, recipe); break;
	case 1: convert_pixels_dst1_src(1, dst, src, len, recipe); break;
	default: abort();
	}
}

void nvnc_pixel_format_to_cpixel(struct nvnc_pixel_format* dst,
		const struct nvnc_pixel_format* src)
{
	*dst = *src;

	if (src->bytes_per_pixel != 4)
		return;

	dst->bytes_per_pixel = UDIV_UP(nvnc_pixel_format_depth(src), 8);
	if (dst->bytes_per_pixel != 3)
		return;

	uint32_t min_shift = dst->red_shift;
	if (min_shift > dst->green_shift)
		min_shift = dst->green_shift;
	if (min_shift > dst->blue_shift)
		min_shift = dst->blue_shift;

	dst->red_shift -= min_shift;
	dst->green_shift -= min_shift;
	dst->blue_shift -= min_shift;
}

void nvnc_convert_pixels(uint8_t* restrict dst,
		const struct nvnc_pixel_format* dst_fmt,
		const uint8_t* restrict src,
		const struct nvnc_pixel_format* src_fmt, size_t len)
{
	assert(src_fmt->bytes_per_pixel <= 4);
	assert(dst_fmt->bytes_per_pixel >= 1 && dst_fmt->bytes_per_pixel <= 4);

	struct nvnc_format_conversion_recipe recipe = {
		.src = src_fmt,
		.dst = dst_fmt,
	};

	switch (recipe.dst->bytes_per_pixel) {
	case 4: convert_pixels_dst4(dst, src, len, &recipe); break;
	case 3: convert_pixels_dst3(dst, src, len, &recipe); break;
	case 2: convert_pixels_dst2(dst, src, len, &recipe); break;
	case 1: convert_pixels_dst1(dst, src, len, &recipe); break;
	default: abort();
	}
}

int nvnc_pixel_format_from_fourcc(struct nvnc_pixel_format* dst, uint32_t src)
{
	assert(!(src & DRM_FORMAT_BIG_ENDIAN));

#define BODY(c1, c2, c3, c4, a, b, c, d) \
	dst->red_shift = RED_SHIFT(c1, c2, c3, c4, a, b, c, d); \
	dst->green_shift = GREEN_SHIFT(c1, c2, c3, c4, a, b, c, d); \
	dst->blue_shift = BLUE_SHIFT(c1, c2, c3, c4, a, b, c, d); \
	dst->bytes_per_pixel = BPP_FROM_SIZES(a, b, c, d) / 8; \
	dst->red_size = RED_SIZE(c1, c2, c3, c4, a, b, c, d); \
	dst->green_size = GREEN_SIZE(c1, c2, c3, c4, a, b, c, d); \
	dst->blue_size = BLUE_SIZE(c1, c2, c3, c4, a, b, c, d); \
	dst->red_max = (1 << RED_SIZE(c1, c2, c3, c4, a, b, c, d)) - 1; \
	dst->green_max = (1 << GREEN_SIZE(c1, c2, c3, c4, a, b, c, d)) - 1; \
	dst->blue_max = (1 << BLUE_SIZE(c1, c2, c3, c4, a, b, c, d)) - 1; \
	break;

	switch (src) {
#define X(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: \
		BODY(c1, c2, c3, c4, a, b, c, d)
#define A X
#define N(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##a##b##c: \
		BODY(c1, c2, c3, c4, a, b, c, d)
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
	default:
		return -1;
	}

#undef BODY

	return 0;
}

void nvnc_pixel_format_from_rfb(struct nvnc_pixel_format* dst,
		const struct rfb_pixel_format* src)
{
	dst->bytes_per_pixel = src->bits_per_pixel / 8;

	dst->red_max = ntohs(src->red_max);
	dst->green_max = ntohs(src->green_max);
	dst->blue_max = ntohs(src->blue_max);

	dst->red_size = POPCOUNT(dst->red_max);
	dst->green_size = POPCOUNT(dst->green_max);
	dst->blue_size = POPCOUNT(dst->blue_max);

	if (src->big_endian_flag) {
		dst->red_shift = src->bits_per_pixel - src->red_shift
			- dst->red_size;
		dst->green_shift = src->bits_per_pixel - src->green_shift
			- dst->green_size;
		dst->blue_shift = src->bits_per_pixel - src->blue_shift
			- dst->blue_size;
	} else {
		dst->red_shift = src->red_shift;
		dst->green_shift = src->green_shift;
		dst->blue_shift = src->blue_shift;
	}
}

void nvnc_pixel_format_to_rfb(struct rfb_pixel_format* dst,
		const struct nvnc_pixel_format* src)
{
	memset(dst, 0, sizeof(*dst));

	dst->bits_per_pixel = src->bytes_per_pixel * 8;
	dst->depth = nvnc_pixel_format_depth(src);
	dst->big_endian_flag = 0;
	dst->true_colour_flag = 1;

	dst->red_max = htons(src->red_max);
	dst->green_max = htons(src->green_max);
	dst->blue_max = htons(src->blue_max);

	dst->red_shift = src->red_shift;
	dst->green_shift = src->green_shift;
	dst->blue_shift = src->blue_shift;
}

int nvnc__pixel_size_from_fourcc(uint32_t fourcc)
{
	assert(!(fourcc & DRM_FORMAT_BIG_ENDIAN));
	switch (fourcc) {
#define X(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: \
		return BPP_FROM_SIZES(a, b, c, d) / 8;
#define A X
#define N(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##a##b##c: \
		return BPP_FROM_SIZES(a, b, c, d) / 8;
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
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
		size_t len, int alpha_shift, uint16_t alpha_max)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t alpha = (src[i] >> alpha_shift) & alpha_max;
		uint8_t binary = !!(alpha > alpha_max / 2);
		dst[i / 8] |= binary << (7 - (i % 8));
	}

	return true;
}

// Note: The destination buffer must be at least UDIV_UP(len, 8) long.
bool extract_alpha_mask(uint8_t* dst, const void* src, uint32_t format,
		size_t len)
{
	assert(!(format & DRM_FORMAT_BIG_ENDIAN));
	memset(dst, 0, UDIV_UP(len, 8));

#define AMAX(c1, c2, c3, c4, a, b, c, d) \
	((1 << ALPHA_SIZE(c1, c2, c3, c4, a, b, c, d)) - 1)

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ASHIFT(c1, c2, c3, c4, a, b, c, d) \
	ALPHA_SHIFT(c1, c2, c3, c4, a, b, c, d)
#else
#define ASHIFT(c1, c2, c3, c4, a, b, c, d) \
	(BPP_FROM_SIZES(a, b, c, d) \
	- ALPHA_SIZE(c1, c2, c3, c4, a, b, c, d) \
	- ALPHA_SHIFT(c1, c2, c3, c4, a, b, c, d))
#endif

	switch (format) {
#define X(...)
#define N(...)
#define A(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: \
		if (BPP_FROM_SIZES(a, b, c, d) == 32) { \
			return extract_alpha_mask_rgba32(dst, src, len, \
					ASHIFT(c1, c2, c3, c4, a, b, c, d), \
					AMAX(c1, c2, c3, c4, a, b, c, d)); \
		} else if (BPP_FROM_SIZES(a, b, c, d) == 16) { \
			return extract_alpha_mask_rgba16(dst, src, len, \
					ASHIFT(c1, c2, c3, c4, a, b, c, d), \
					AMAX(c1, c2, c3, c4, a, b, c, d)); \
		} \
		break;
	PIXEL_FORMAT_TABLE
#undef A
#undef N
#undef X
	}

#undef ASHIFT
#undef AMAX

	return false;
}

const char* drm_format_to_string(uint32_t fmt)
{
	switch (fmt) {
#define X(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: \
		return XSTR(c1##c2##c3##c4##a##b##c##d);
#define A X
#define N(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##a##b##c: \
		return XSTR(c1##c2##c3##a##b##c);
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
	}
	return "UNKNOWN";
}

const char* nvnc_pixel_format_to_string(const struct nvnc_pixel_format* fmt)
{
	uint32_t profile = (fmt->bytes_per_pixel * 8 << 24)
		| (fmt->red_shift << 16) | (fmt->green_shift << 8)
		| (fmt->blue_shift);

#define FMT_PROFILE(c1, c2, c3, c4, a, b, c, d) \
	((BPP_FROM_SIZES(a, b, c, d) << 24) \
	| (RED_SHIFT(c1, c2, c3, c4, a, b, c, d) << 16) \
	| (GREEN_SHIFT(c1, c2, c3, c4, a, b, c, d) << 8) \
	| BLUE_SHIFT(c1, c2, c3, c4, a, b, c, d))

	switch (profile) {
#define A(...)
#define X(c1, c2, c3, c4, a, b, c, d) \
	case FMT_PROFILE(c1, c2, c3, c4, a, b, c, d): \
		return XSTR(c1##c2##c3##c4##a##b##c##d);
#define N(c1, c2, c3, c4, a, b, c, d) \
	case FMT_PROFILE(c1, c2, c3, c4, a, b, c, d): \
		return XSTR(c1##c2##c3##a##b##c);
	PIXEL_FORMAT_TABLE
#undef N
#undef X
#undef A
	}

#undef FMT_PROFILE

	return "UNKNOWN";
}

void make_rgb332_pal8_map(struct rfb_set_colour_map_entries_msg* msg)
{
	msg->type = RFB_SERVER_TO_CLIENT_SET_COLOUR_MAP_ENTRIES;
	msg->padding = 0;
	msg->first_colour = htons(0);
	msg->n_colours = htons(256);

	for (unsigned int i = 0; i < 256; ++i) {
		msg->colours[i].r = htons(round(65535.0 / 7.0 * ((i >> 5) & 7)));
		msg->colours[i].g = htons(round(65535.0 / 7.0 * ((i >> 2) & 7)));
		msg->colours[i].b = htons(round(65535.0 / 3.0 * (i & 3)));
	}
}

static int get_format_depth(uint32_t format)
{
	switch (format) {
#define X(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: \
		return RGB_DEPTH(c1, c2, c3, c4, a, b, c, d);
#define A X
#define N(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##a##b##c: \
		return RGB_DEPTH(c1, c2, c3, c4, a, b, c, d);
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
	}
	return 0;
}

/*
 *     ^  score
 *     |
 * 1.0 +.....,
 *     |     |\
 *     |     | \
 *     |     |  \
 *     |     |   \
 *     |    /.    .
 *     |   / .    .
 *     |  /  .    .
 *     | /   .    .
 *     |/    .    .
 *     +-----+----+---> depth
 *            \    `- max_depth
 *             `- target_depth
 */
static double rate_format_by_depth(uint32_t format, int target_depth)
{
	int depth = get_format_depth(format);
	if (depth == 0)
		return 0;

	const double max_depth = 30;

	if (depth >= target_depth) {
		return (target_depth + max_depth - depth) / max_depth;
	}

	return depth / max_depth;
}

static bool format_has_alpha(uint32_t format)
{
	switch (format) {
#define X(...)
#define A(c1, c2, c3, c4, a, b, c, d) \
	case DRM_FORMAT_##c1##c2##c3##c4##a##b##c##d: return true;
#define N(...)
	PIXEL_FORMAT_TABLE
#undef N
#undef A
#undef X
	}
	return false;
}

int nvnc_pixel_format_depth(const struct nvnc_pixel_format* fmt)
{
	return fmt->red_size + fmt->green_size + fmt->blue_size;
}

// All AMD modifiers except DCC are allowed
static bool amd_format_modifier_is_allowed(uint64_t modifier)
{
	return !AMD_FMT_MOD_GET(DCC, modifier) &&
		!AMD_FMT_MOD_GET(DCC_RETILE, modifier);
}

static const uint64_t format_modifier_allow_list[] = {
	DRM_FORMAT_MOD_LINEAR,

	// Intel:
	I915_FORMAT_MOD_X_TILED,
	I915_FORMAT_MOD_Y_TILED,
	I915_FORMAT_MOD_Yf_TILED,
	// I915_FORMAT_MOD_4_TILED might work but is untested
};

static bool format_modifier_is_allowed(uint64_t modifier)
{
	if (fourcc_mod_is_vendor(modifier, AMD))
		return amd_format_modifier_is_allowed(modifier);

	for (size_t i = 0; i < sizeof(format_modifier_allow_list) /
			sizeof(format_modifier_allow_list[0]); ++i)
		if (modifier == format_modifier_allow_list[i])
			return true;

	return false;
}

double rate_pixel_format(uint32_t format, uint64_t modifier,
		enum format_rating_flags flags, int target_depth)
{
	double depth_rating = rate_format_by_depth(format, target_depth);
	if (depth_rating == 0)
		return 0;

	if (!format_modifier_is_allowed(modifier))
		return 0;

	double linear_rating = modifier == DRM_FORMAT_MOD_LINEAR;

	double alpha_rating;
	if (flags & FORMAT_RATING_NEED_ALPHA) {
		alpha_rating = format_has_alpha(format);
		if (alpha_rating == 0)
			return 0;
	} else {
		alpha_rating = !format_has_alpha(format);
	}

	const double depth_weight = 100;
	const double linear_weight = (flags & FORMAT_RATING_PREFER_LINEAR) ? 10 : 0;
	const double alpha_weight = 1;

	const double total_weight = depth_weight + linear_weight + alpha_weight;
	if (total_weight == 0)
		return 0;

	return (depth_weight * depth_rating
	     + linear_weight * linear_rating
	     + alpha_weight * alpha_rating) / total_weight;
}
