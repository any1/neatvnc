#include "rfb-proto.h"
#include "bitmap.h"
#include "util.h"
#include "vec.h"

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <endian.h>
#include <assert.h>
#include <zlib.h>
#include <uv.h>
#include <pixman.h>

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

void zrle_encode_tile(struct vec *dst, const struct rfb_pixel_format *dst_fmt,
		      const uint32_t *src,
		      const struct rfb_pixel_format *src_fmt,
		      int stride, int width, int height)
{
	int bytes_per_cpixel = dst_fmt->depth / 8;

	vec_clear(dst);

	vec_fast_append_8(dst, 0);

	for (int y = 0; y < height; ++y)
		pixel32_to_cpixel(((uint8_t*)dst->data) + 1 + width * y * bytes_per_cpixel,
				  dst_fmt, src + stride * y,
				  src_fmt, bytes_per_cpixel, width);

	dst->len += bytes_per_cpixel * width * height;
}

int zrle_encode_box(uv_stream_t *stream, const struct rfb_pixel_format *dst_fmt,
		    uint8_t *src, const struct rfb_pixel_format *src_fmt,
		    int x, int y, int stride, int width, int height)
{
	int r = -1;
	int zr = Z_STREAM_ERROR;
	int bytes_per_cpixel = dst_fmt->depth / 8;
	int chunk_size = 1 + bytes_per_cpixel * 64 * 64;
	z_stream zs = { 0 };

	struct vec out;
	struct vec in;

	if (vec_init(&in, 1 + bytes_per_cpixel * 64 * 64) < 0)
		goto failure;

	if (vec_init(&out, bytes_per_cpixel * width * height * 2) < 0)
		goto failure;

	r = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
	if (r != Z_OK)
		goto failure;

	/* Reserve space for size */
	vec_append_zero(&out, 4);

	int n_tiles = UDIV_UP(width, 64) * UDIV_UP(height, 64);

	for (int i = 0; i < n_tiles; ++i) {
		int tile_x = x + (i % UDIV_UP(width, 64)) * 64;
		int tile_y = y + (i / UDIV_UP(width, 64)) * 64;

		int tile_width = width - tile_x >= 64 ? 64 : width - tile_x;
		int tile_height = height - tile_y >= 64 ? 64 : height - tile_y;

		printf("Encoding tile @ %dx%d. width: %d, height: %d\n", tile_x,
				tile_y, tile_width, tile_height);

		zrle_encode_tile(&in, dst_fmt,
				 ((uint32_t*)src) + tile_x + tile_y * width,
				 src_fmt, stride, tile_width, tile_height);

		zs.next_in = in.data;
		zs.avail_in = in.len;

		int flush = (i == n_tiles - 1) ? Z_FINISH : Z_NO_FLUSH;

		do {
			/*
			r = vec_reserve(&out, out.len + chunk_size);
			if (r < 0)
				goto failure;
*/

			zs.next_out = ((Bytef*)out.data) + out.len;
			zs.avail_out = out.cap - out.len;

			zr = deflate(&zs, flush);
			assert(zr != Z_STREAM_ERROR);

			out.len = out.cap - zs.avail_out;
		} while (zs.avail_out == 0);

		assert(zs.avail_in == 0);
	}

	assert(zr == Z_STREAM_END);

	deflateEnd(&zs);

	uint32_t *out_size = out.data;
	*out_size = htonl(out.len - 8);

	printf("Sending %lu bytes\n", out.len - 4);

	r = vnc__write(stream, out.data, out.len - 4, NULL);
failure:
//	vec_destroy(&out);
	vec_destroy(&in);
	return r;
#undef CHUNK
}

int zrle_encode_frame(uv_stream_t *stream,
		      const struct rfb_pixel_format *dst_fmt,
		      uint8_t *src,
		      const struct rfb_pixel_format *src_fmt,
		      int width, int height,
		      struct pixman_region16 *region)
{
	int rc = -1;

	int n_rects = 0;
	struct pixman_box16 *box = pixman_region_rectangles(region, &n_rects);
	if (n_rects > UINT16_MAX) {
		box = pixman_region_extents(region);
		n_rects = 1;
	}

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(n_rects),
	};

	rc = vnc__write(stream, &head, sizeof(head), NULL);
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_ZRLE),
			.x = htons(x),
			.y = htons(y),
			.width = htons(box_width),
			.height = htons(box_height),
		};

		rc = vnc__write(stream, &rect, sizeof(rect), NULL);
		if (rc < 0)
			return -1;

		rc = zrle_encode_box(stream, dst_fmt, src, src_fmt, x, y,
				     width, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	return 0;
}
