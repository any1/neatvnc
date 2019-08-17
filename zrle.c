#include "rfb-proto.h"

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <endian.h>
#include <assert.h>
#include <zlib.h>

#define POPCOUNT(x) __builtin_popcount(x)

struct bitmap;

size_t bitmap_popcount(struct bitmap*);

#define bitmap_for_each(b)

void pixel_format_into_native(struct rfb_pixel_format *fmt)
{
#if __BYTE_ORDER__ == __LITTLE_ENDIAN__
	if (!fmt->big_endian_flag)
		return;

	fmt->big_endian_flag = 0;
#else
	if (fmt->big_endian_flag)
		return;

	fmt->big_endian_flag = 1;
#endif

	fmt->red_shift = fmt->bits_per_pixel - fmt->red_shift;
	fmt->green_shift = fmt->bits_per_pixel - fmt->green_shift;
	fmt->blue_shift = fmt->bits_per_pixel - fmt->blue_shift;
}

void pixel32_to_cpixel(uint8_t *restrict dst,
		       const struct rfb_pixel_format* dst_fmt,
		       const uint32_t *restrict src,
		       const struct rfb_pixel_format* src_fmt,
		       size_t bytes_per_cpixel, size_t len)
{
	assert(src_fmt->true_colour_flag);
	assert(src_fmt->bits_per_pixel == 32);
	assert(src_fmt->depth <= 32);
	assert(dst_fmt->true_colour_flag);
	assert(dst_fmt->bits_per_pixel <= 32);
	assert(dst_fmt->depth <= 24);
	assert(bytes_per_cpixel <= 3 && bytes_per_cpixel >= 1);

	uint32_t src_red_shift = src_fmt->red_shift;
	uint32_t src_green_shift = src_fmt->green_shift;
	uint32_t src_blue_shift = src_fmt->blue_shift;

	uint32_t dst_red_shift = dst_fmt->red_shift;
	uint32_t dst_green_shift = dst_fmt->green_shift;
	uint32_t dst_blue_shift = dst_fmt->blue_shift;

	uint32_t src_red_max = src_fmt->red_max;
	uint32_t src_green_max = src_fmt->green_max;
	uint32_t src_blue_max = src_fmt->blue_max;

	uint32_t dst_red_max = dst_fmt->red_max;
	uint32_t dst_green_max = dst_fmt->green_max;
	uint32_t dst_blue_max = dst_fmt->blue_max;

	uint32_t src_red_bits = POPCOUNT(src_fmt->red_max);
	uint32_t src_green_bits = POPCOUNT(src_fmt->green_max);
	uint32_t src_blue_bits = POPCOUNT(src_fmt->blue_max);

	uint32_t dst_red_bits = POPCOUNT(dst_fmt->red_max);
	uint32_t dst_green_bits = POPCOUNT(dst_fmt->green_max);
	uint32_t dst_blue_bits = POPCOUNT(dst_fmt->blue_max);

	uint32_t dst_endian_correction;

#define CONVERT_PIXELS(cpx, px) \
	{ \
		uint32_t r, g, b; \
		r = ((px >> src_red_shift) & src_red_max) << dst_red_bits \
			>> src_red_bits << dst_red_shift; \
		g = ((px >> src_green_shift) & src_green_max) << dst_green_bits \
			>> src_green_bits << dst_green_shift; \
		b = ((px >> src_blue_shift) & src_blue_max) << dst_blue_bits \
			>> src_blue_bits << dst_blue_shift; \
		cpx = r | g | b; \
	}

	switch (bytes_per_cpixel) {
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

void zrle_encode_tile(uint8_t *dst, const struct rfb_pixel_format *dst_fmt,
		      const uint32_t *src,
		      const struct rfb_pixel_format *src_fmt,
		      int stride, int width, int height)
{
	int bytes_per_cpixel = dst_fmt->depth / 8;

	for (int y = 0; y < height; ++y)
		pixel32_to_cpixel(dst + width * y, dst_fmt, src + stride * y,
				  src_fmt, bytes_per_cpixel, width);
}

ssize_t zrle_encode_frame(uint8_t **result,
			  const struct rfb_pixel_format *dst_fmt,
			  uint8_t *src,
			  const struct rfb_pixel_format *src_fmt,
			  int width, int height,
			  struct bitmap *tile_mask)
{
	/* Expect the compressed data to be half the size of the updated regions
	 */
	size_t actual_size, size = 64 * bitmap_popcount(tile_mask) / 2;
	int n_tiles = UDIV_UP(width, 64) * UDIV_UP(height, 64);

	uint8_t *frame = malloc(size);
	if (!frame)
		return -1;

	int bytes_per_cpixel = dst_fmt->depth / 8;

	uint8_t *tile_buffer = malloc(size * bytes_per_cpixel);
	if (!tile_buffer)
		goto failure;

	int boff = 0;

	for (int i = 0; i < n_tiles;) {
		if (!bitmap_is_set(tile_mask, i))
			continue;

		int x = (i * 64) % UDIV_UP(width, 64);
		int y = (i * 64) / UDIV_UP(width, 64);

	}

	*result = frame;
	return actual_size;

failure:
	free(frame);
	return -1;
}
