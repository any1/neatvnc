/*
 * Copyright (c) 2019 Andri Yngvason
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
#include "vec.h"
#include "zrle.h"
#include "miniz.h"
#include "neatvnc.h"
#include "pixels.h"
#include "fb.h"

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <uv.h>
#include <pixman.h>

#define TILE_LENGTH 64

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

static inline int find_colour_in_palette(uint32_t* palette, int len,
                                         uint32_t colour)
{
	for (int i = 0; i < len; ++i)
		if (palette[i] == colour)
			return i;

	return -1;
}

static inline int calc_bytes_per_cpixel(const struct rfb_pixel_format* fmt)
{
	return fmt->bits_per_pixel == 32 ? fmt->depth / 8
	                                 : fmt->bits_per_pixel / 8;
}

int zrle_get_tile_palette(uint32_t* palette, const uint32_t* src, size_t length)
{
	int n = 0;

	/* TODO: Maybe ignore the alpha channel */
	palette[n++] = src[0];

	for (size_t i = 0; i < length; ++i) {
		uint32_t colour = src[i];

		if (find_colour_in_palette(palette, n, colour) < 0) {
			if (n >= 16)
				return -1;

			palette[n++] = colour;
		}
	}

	return n;
}

void zrle_encode_unichrome_tile(struct vec* dst,
                                const struct rfb_pixel_format* dst_fmt,
                                uint32_t colour,
                                const struct rfb_pixel_format* src_fmt)
{
	int bytes_per_cpixel = calc_bytes_per_cpixel(dst_fmt);

	vec_fast_append_8(dst, 1);

	pixel32_to_cpixel(((uint8_t*)dst->data) + 1, dst_fmt, &colour, src_fmt,
	                  bytes_per_cpixel, 1);

	dst->len += bytes_per_cpixel;
}

void encode_run_length(struct vec* dst, uint8_t index, int run_length)
{
	if (run_length == 1) {
		vec_fast_append_8(dst, index);
		return;
	}

	vec_fast_append_8(dst, index | 128);

	while (run_length > 255) {
		vec_fast_append_8(dst, 255);
		run_length -= 255;
	}

	vec_fast_append_8(dst, run_length - 1);
}

void zrle_encode_packed_tile(struct vec* dst,
                             const struct rfb_pixel_format* dst_fmt,
                             const uint32_t* src,
                             const struct rfb_pixel_format* src_fmt,
                             size_t length, uint32_t* palette, int palette_size)
{
	int bytes_per_cpixel = calc_bytes_per_cpixel(dst_fmt);

	uint8_t cpalette[16 * 3];
	pixel32_to_cpixel((uint8_t*)cpalette, dst_fmt, palette, src_fmt,
	                  bytes_per_cpixel, palette_size);

	vec_fast_append_8(dst, 128 | palette_size);

	vec_append(dst, cpalette, palette_size * bytes_per_cpixel);

	int index;
	int run_length = 1;

	for (size_t i = 1; i < length; ++i) {
		if (src[i] == src[i - 1]) {
			run_length++;
			continue;
		}

		index = find_colour_in_palette(palette, palette_size, src[i - 1]);
		encode_run_length(dst, index, run_length);
		run_length = 1;
	}

	if (run_length > 0) {
		index = find_colour_in_palette(palette, palette_size,
		                               src[length - 1]);
		encode_run_length(dst, index, run_length);
	}
}

void zrle_copy_tile(uint32_t* dst, const uint32_t* src, int stride, int width,
                    int height)
{
	for (int y = 0; y < height; ++y)
		memcpy(dst + y * width, src + y * stride, width * 4);
}

void zrle_encode_tile(struct vec* dst, const struct rfb_pixel_format* dst_fmt,
                      const uint32_t* src,
                      const struct rfb_pixel_format* src_fmt, size_t length)
{
	int bytes_per_cpixel = calc_bytes_per_cpixel(dst_fmt);

	vec_clear(dst);

	uint32_t palette[16];
	int palette_size = zrle_get_tile_palette(palette, src, length);

	if (palette_size == 1) {
		zrle_encode_unichrome_tile(dst, dst_fmt, palette[0], src_fmt);
		return;
	}

	if (palette_size > 1) {
		zrle_encode_packed_tile(dst, dst_fmt, src, src_fmt, length,
		                        palette, palette_size);
		return;
	}

	vec_fast_append_8(dst, 0);

	pixel32_to_cpixel(((uint8_t*)dst->data) + 1, dst_fmt, src, src_fmt,
	                  bytes_per_cpixel, length);

	dst->len += bytes_per_cpixel * length;
}

int zrle_deflate(struct vec* dst, const struct vec* src, z_stream* zs, bool flush)
{
	int r = Z_STREAM_ERROR;

	zs->next_in = src->data;
	zs->avail_in = src->len;

	do {
		if (dst->len == dst->cap && vec_reserve(dst, dst->cap * 2) < 0)
			return -1;

		zs->next_out = ((Bytef*)dst->data) + dst->len;
		zs->avail_out = dst->cap - dst->len;

		r = deflate(zs, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
		assert(r != Z_STREAM_ERROR);

		dst->len = zs->next_out - (Bytef*)dst->data;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}

int zrle_encode_box(struct vec* out, const struct rfb_pixel_format* dst_fmt,
                    const struct nvnc_fb* fb,
                    const struct rfb_pixel_format* src_fmt, int x, int y,
                    int stride, int width, int height, z_stream* zs)
{
	int r = -1;
	int bytes_per_cpixel = calc_bytes_per_cpixel(dst_fmt);
	struct vec in;

	uint32_t* tile = malloc(TILE_LENGTH * TILE_LENGTH * 4);
	if (!tile)
		goto failure;

	if (vec_init(&in, 1 + bytes_per_cpixel * TILE_LENGTH * TILE_LENGTH) < 0)
		goto failure;

	struct rfb_server_fb_rect rect = {
	        .encoding = htonl(RFB_ENCODING_ZRLE),
	        .x = htons(x),
	        .y = htons(y),
	        .width = htons(width),
	        .height = htons(height),
	};

	r = vec_append(out, &rect, sizeof(rect));
	if (r < 0)
		goto failure;

	/* Reserve space for size */
	size_t size_index = out->len;
	vec_append_zero(out, 4);

	int n_tiles = UDIV_UP(width, TILE_LENGTH) * UDIV_UP(height, TILE_LENGTH);

	for (int i = 0; i < n_tiles; ++i) {
		int tile_x = (i % UDIV_UP(width, TILE_LENGTH)) * TILE_LENGTH;
		int tile_y = (i / UDIV_UP(width, TILE_LENGTH)) * TILE_LENGTH;

		int tile_width = width - tile_x >= TILE_LENGTH ? TILE_LENGTH
		                                               : width - tile_x;
		int tile_height = height - tile_y >= TILE_LENGTH
		                          ? TILE_LENGTH
		                          : height - tile_y;

		int y_off = y + tile_y;

		zrle_copy_tile(tile,
		               ((uint32_t*)fb->addr) + x + tile_x + y_off * stride,
		               stride, tile_width, tile_height);

		zrle_encode_tile(&in, dst_fmt, tile, src_fmt,
		                 tile_width * tile_height);

		r = zrle_deflate(out, &in, zs, i == n_tiles - 1);
		if (r < 0)
			goto failure;
	}

	uint32_t out_size = htonl(out->len - size_index - 4);
	memcpy(((uint8_t*)out->data) + size_index, &out_size, sizeof(out_size));

failure:
	vec_destroy(&in);
	free(tile);
	return r;
#undef CHUNK
}

int zrle_encode_frame(z_stream* zs, struct vec* dst,
                      const struct rfb_pixel_format* dst_fmt,
                      const struct nvnc_fb* src,
                      const struct rfb_pixel_format* src_fmt,
                      struct pixman_region16* region)
{
	int rc = -1;

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(region, &n_rects);
	if (n_rects > UINT16_MAX) {
		box = pixman_region_extents(region);
		n_rects = 1;
	}

	struct rfb_server_fb_update_msg head = {
	        .type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
	        .n_rects = htons(n_rects),
	};

	rc = vec_append(dst, &head, sizeof(head));
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = zrle_encode_box(dst, dst_fmt, src, src_fmt, x, y,
		                     src->width, box_width, box_height, zs);
		if (rc < 0)
			return -1;
	}

	return 0;
}
