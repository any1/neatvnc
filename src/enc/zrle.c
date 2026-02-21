/*
 * Copyright (c) 2019 - 2025 Andri Yngvason
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
#include "neatvnc.h"
#include "pixels.h"
#include "fb.h"
#include "enc/util.h"
#include "enc/encoder.h"
#include "parallel-deflate.h"

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <pixman.h>
#include <aml.h>

#define TILE_LENGTH 64

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

struct encoder* zrle_encoder_new(void);

struct zrle_encoder {
	struct encoder encoder;

	struct rfb_pixel_format output_format;

	struct nvnc_composite_fb current_fb;
	struct pixman_region16 current_damage;

	struct encoded_frame* current_result;
	int n_rects;

	struct parallel_deflate* zs;

	struct aml_work* work;
};

struct encoder_impl encoder_impl_zrle;

static inline struct zrle_encoder* zrle_encoder(struct encoder* encoder)
{
	assert(encoder->impl == &encoder_impl_zrle);
	return (struct zrle_encoder*)encoder;
}

static inline int find_colour_in_palette(uint8_t* palette, int len,
		const uint8_t* colour, int bpp)
{
	for (int i = 0; i < len; ++i)
		if (memcmp(palette + i * bpp, colour, bpp) == 0)
			return i;

	return -1;
}

static int zrle_get_tile_palette(uint8_t* palette, const uint8_t* src,
		const int src_bpp, size_t length)
{
	int n = 0;

	/* TODO: Maybe ignore the alpha channel */
	memcpy(palette + (n++ * src_bpp), src, src_bpp);

	for (size_t i = 0; i < length; ++i) {
		const uint8_t* colour_addr = src + i * src_bpp;

		if (find_colour_in_palette(palette, n, colour_addr, src_bpp) < 0) {
			if (n >= 16)
				return -1;

			memcpy(palette + (n++ * src_bpp), colour_addr, src_bpp);
		}
	}

	return n;
}

static void zrle_encode_unichrome_tile(struct vec* dst,
		const struct rfb_pixel_format* dst_fmt,
		uint8_t* colour,
		const struct rfb_pixel_format* src_fmt)
{
	int bytes_per_cpixel = nvnc__calc_bytes_per_cpixel(dst_fmt);

	vec_fast_append_8(dst, 1);

	pixel_to_cpixel(((uint8_t*)dst->data) + 1, dst_fmt, colour, src_fmt,
			bytes_per_cpixel, 1);

	dst->len += bytes_per_cpixel;
}

static void encode_run_length(struct vec* dst, uint8_t index, int run_length)
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

static void zrle_encode_packed_tile(struct vec* dst,
		const struct rfb_pixel_format* dst_fmt,
		const uint8_t* src,
		const struct rfb_pixel_format* src_fmt,
		size_t length, uint8_t* palette,
		int palette_size)
{
	int bytes_per_cpixel = nvnc__calc_bytes_per_cpixel(dst_fmt);
	int src_bpp = src_fmt->bits_per_pixel / 8;

	uint8_t cpalette[16 * 4];
	pixel_to_cpixel(cpalette, dst_fmt, palette, src_fmt,
			bytes_per_cpixel, palette_size);

	vec_fast_append_8(dst, 128 | palette_size);

	vec_append(dst, cpalette, palette_size * bytes_per_cpixel);

	int index;
	int run_length = 1;

	for (size_t i = 1; i < length; ++i) {
		if (memcmp(src + i * src_bpp, src + (i - 1) * src_bpp, src_bpp) == 0) {
			run_length++;
			continue;
		}

		index = find_colour_in_palette(palette, palette_size, src + (i - 1) * src_bpp, src_bpp);
		encode_run_length(dst, index, run_length);
		run_length = 1;
	}

	if (run_length > 0) {
		index = find_colour_in_palette(palette, palette_size,
				src + (length - 1) * src_bpp, src_bpp);
		encode_run_length(dst, index, run_length);
	}
}

static void zrle_copy_tile(uint8_t* tile, const uint8_t* src, int src_bpp,
		int stride, int width, int height)
{
	int byte_stride = stride * src_bpp;
	for (int y = 0; y < height; ++y)
		memcpy(tile + y * width * src_bpp, src + y * byte_stride, width * src_bpp);
}

static void zrle_encode_tile(struct vec* dst,
		const struct rfb_pixel_format* dst_fmt,
		const uint8_t* src,
		const struct rfb_pixel_format* src_fmt,
		size_t length)
{
	int bytes_per_cpixel = nvnc__calc_bytes_per_cpixel(dst_fmt);
	int src_bpp = src_fmt->bits_per_pixel / 8;
	vec_clear(dst);

	uint8_t palette[16 * 4];
	int palette_size = zrle_get_tile_palette(palette, src, src_bpp, length);

	if (palette_size == 1) {
		zrle_encode_unichrome_tile(dst, dst_fmt, &palette[0], src_fmt);
		return;
	}

	if (palette_size > 1) {
		int len_before = dst->len;
		zrle_encode_packed_tile(dst, dst_fmt, src, src_fmt,
				length, palette, palette_size);

		if (dst->len - len_before <= 1 + bytes_per_cpixel * length)
			return;

		// If a packed tile is bigger, we don't want to use it.
		dst->len = len_before;
	}

	vec_fast_append_8(dst, 0);

	pixel_to_cpixel(((uint8_t*)dst->data) + 1, dst_fmt, (uint8_t*)src, src_fmt,
			bytes_per_cpixel, length);

	dst->len += bytes_per_cpixel * length;
}

static int zrle_encode_box(struct zrle_encoder* self, struct vec* out,
		const struct rfb_pixel_format* dst_fmt,
		const struct nvnc_fb* fb,
		const struct rfb_pixel_format* src_fmt, int x, int y,
		int stride, int width, int height)
{
	int r = -1;
	int bytes_per_cpixel = nvnc__calc_bytes_per_cpixel(dst_fmt);
	int src_bpp = src_fmt->bits_per_pixel / 8;
	struct vec in;

	uint16_t x_pos = fb->x_off;
	uint16_t y_pos = fb->y_off;

	uint8_t* tile = malloc(TILE_LENGTH * TILE_LENGTH * 4);
	if (!tile)
		goto failure;

	if (vec_init(&in, 1 + bytes_per_cpixel * TILE_LENGTH * TILE_LENGTH +
				16 * 4) < 0)
		goto failure;

	r = nvnc__encode_rect_head(out, RFB_ENCODING_ZRLE, x_pos + x, y_pos + y,
			width, height);
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

		int y_off = (y + tile_y) * stride * src_bpp;
		int x_off = (x + tile_x) * src_bpp;

		zrle_copy_tile(tile,
				((uint8_t*)fb->addr) + x_off + y_off, src_bpp,
				stride, tile_width, tile_height);

		zrle_encode_tile(&in, dst_fmt, tile, src_fmt,
				tile_width * tile_height);

		parallel_deflate_feed(self->zs, out, in.data, in.len);
	}

	parallel_deflate_sync(self->zs, out);

	uint32_t out_size = htonl(out->len - size_index - 4);
	memcpy(((uint8_t*)out->data) + size_index, &out_size, sizeof(out_size));

failure:
	vec_destroy(&in);
	free(tile);
	return r;
#undef CHUNK
}

static int zrle_encode_frame(struct zrle_encoder* self,
		struct vec* dst, const struct rfb_pixel_format* dst_fmt,
		struct nvnc_fb* src, const struct rfb_pixel_format* src_fmt,
		struct pixman_region16* region)
{
	int rc __attribute__((unused)) = -1;

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(region, &n_rects);

	rc = nvnc_fb_map(src);
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = zrle_encode_box(self, dst, dst_fmt, src, src_fmt,
				x - src->x_off, y - src->y_off,
				src->stride, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	self->n_rects += n_rects;
	return 0;
}

static void zlre_encoder_init_damage_subregions(struct zrle_encoder* self,
		struct pixman_region16 subregions[])
{
	struct nvnc_composite_fb* cfb = &self->current_fb;

	int n_rects = 0;
	for (int i = 0; i < cfb->n_fbs; ++i) {
		struct nvnc_fb* fb = cfb->fbs[i];
		assert(fb);

		pixman_region_init(&subregions[i]);
		pixman_region_intersect_rect(&subregions[i],
				&self->current_damage, fb->x_off, fb->y_off,
				fb->width, fb->height);
		n_rects += pixman_region_n_rects(&subregions[i]);
	}

	if (n_rects > UINT16_MAX) {
		n_rects = cfb->n_fbs;

		for (int i = 0; i < cfb->n_fbs; ++i) {
			struct nvnc_fb* fb = cfb->fbs[i];
			assert(fb);
			pixman_region_fini(&subregions[i]);
			pixman_region_init_rect(&subregions[i], fb->x_off,
					fb->y_off, fb->width, fb->height);
		}
	}
}

static int zrle_encoder_alloc_output_buffer(struct zrle_encoder* self,
		struct vec* dst)
{
	int n_rects = pixman_region_n_rects(&self->current_damage);
	size_t bpp = self->output_format.bits_per_pixel / 8;
	size_t buffer_size = nvnc__calculate_region_area(&self->current_damage) * bpp
		+ n_rects * sizeof(struct rfb_server_fb_rect);

	return vec_init(dst, buffer_size);
}

static void zrle_encoder_do_work(struct aml_work* work)
{
	struct zrle_encoder* self = aml_get_userdata(work);
	int rc;

	struct nvnc_composite_fb* cfb = &self->current_fb;
	assert(cfb->n_fbs != 0);

	struct pixman_region16 subregions[NVNC_FB_COMPOSITE_MAX] = { 0 };
	zlre_encoder_init_damage_subregions(self, subregions);

	struct vec dst;
	nvnc_assert(zrle_encoder_alloc_output_buffer(self, &dst) >= 0, "OOM");

	self->n_rects = 0;

	for (int i = 0; i < cfb->n_fbs; ++i) {
		struct nvnc_fb* fb = cfb->fbs[i];
		assert(fb);

		struct rfb_pixel_format src_fmt;
		rc = rfb_pixfmt_from_fourcc(&src_fmt, nvnc_fb_get_fourcc_format(fb));
		nvnc_assert(rc == 0, "Unsupported pixel format");

		rc = zrle_encode_frame(self, &dst, &self->output_format, fb,
				&src_fmt, &subregions[i]);
		nvnc_assert(rc == 0, "Failed to encode frame");
	}

	uint16_t width = nvnc_composite_fb_width(cfb);
	uint16_t height = nvnc_composite_fb_height(cfb);
	uint64_t pts = nvnc_composite_fb_pts(cfb);

	self->current_result = nvnc__encoded_frame_new(dst.data, dst.len,
			self->n_rects, width, height, pts);
	assert(self->current_result);

	for (int i = 0; i < cfb->n_fbs; ++i)
		pixman_region_fini(&subregions[i]);
}

static void zrle_encoder_on_done(struct aml_work* work)
{
	struct zrle_encoder* self = aml_get_userdata(work);

	assert(self->current_result);

	nvnc_composite_fb_release(&self->current_fb);
	nvnc_composite_fb_unref(&self->current_fb);
	memset(&self->current_fb, 0, sizeof(self->current_fb));

	pixman_region_clear(&self->current_damage);

	struct encoded_frame* result = self->current_result;
	self->current_result = NULL;

	aml_unref(self->work);
	self->work = NULL;

	encoder_finish_frame(&self->encoder, result);

	encoded_frame_unref(result);
	encoder_unref(&self->encoder);
}

struct encoder* zrle_encoder_new(void)
{
	struct zrle_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	encoder_init(&self->encoder, &encoder_impl_zrle);

	int level = 1;
	int window_bits = -15;
	int mem_level = 9;
	int strategy = Z_DEFAULT_STRATEGY;

	self->zs = parallel_deflate_new(level, window_bits, mem_level,
			strategy);
	if (!self->zs)
		goto deflate_failure;

	pixman_region_init(&self->current_damage);

	aml_require_workers(aml_get_default(), 2);

	return (struct encoder*)self;

deflate_failure:
	free(self);
	return NULL;
}

static void zrle_encoder_destroy(struct encoder* encoder)
{
	struct zrle_encoder* self = zrle_encoder(encoder);
	pixman_region_fini(&self->current_damage);
	parallel_deflate_destroy(self->zs);
	if (self->work)
		aml_unref(self->work);
	if (self->current_result)
		encoded_frame_unref(self->current_result);
	free(self);
}

static void zrle_encoder_set_output_format(struct encoder* encoder,
		const struct rfb_pixel_format* pixfmt)
{
	struct zrle_encoder* self = zrle_encoder(encoder);
	memcpy(&self->output_format, pixfmt, sizeof(self->output_format));
}

static int zrle_encoder_encode(struct encoder* encoder,
		struct nvnc_composite_fb* fb, struct pixman_region16* damage)
{
	struct zrle_encoder* self = zrle_encoder(encoder);

	assert(self->current_fb.n_fbs == 0);

	self->work = aml_work_new(zrle_encoder_do_work, zrle_encoder_on_done,
			self, NULL);
	if (!self->work)
		return -1;

	nvnc_composite_fb_copy(&self->current_fb, fb);
	nvnc_composite_fb_hold(&self->current_fb);
	pixman_region_copy(&self->current_damage, damage);

	encoder_ref(&self->encoder);

	int rc = aml_start(aml_get_default(), self->work);
	nvnc_assert(rc == 0, "Failed to start encoding job");

	return rc;
}

struct encoder_impl encoder_impl_zrle = {
	.destroy = zrle_encoder_destroy,
	.set_output_format = zrle_encoder_set_output_format,
	.encode = zrle_encoder_encode,
};
