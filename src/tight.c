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

#include "neatvnc.h"
#include "rfb-proto.h"
#include "common.h"
#include "pixels.h"
#include "vec.h"
#include "tight.h"
#include "config.h"
#include "enc-util.h"
#include "fb.h"
#include "rcbuf.h"
#include "encoder.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zlib.h>
#include <pixels.h>
#include <pthread.h>
#include <assert.h>
#include <aml.h>
#include <libdrm/drm_fourcc.h>
#ifdef HAVE_JPEG
#include <turbojpeg.h>
#endif

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

#define TIGHT_FILL 0x80
#define TIGHT_JPEG 0x90
#define TIGHT_PNG 0xA0
#define TIGHT_BASIC 0x00

#define TIGHT_STREAM(n) ((n) << 4)
#define TIGHT_RESET(n) (1 << (n))

#define TSL 64 /* Tile Side Length */

#define MAX_TILE_SIZE (2 * TSL * TSL * 4)

struct encoder* tight_encoder_new(uint16_t width, uint16_t height);

typedef void (*tight_done_fn)(struct vec* frame, void*);

struct tight_encoder {
	struct encoder encoder;

	uint32_t width;
	uint32_t height;
	uint32_t grid_width;
	uint32_t grid_height;
	enum tight_quality quality;

	struct tight_tile* grid;

	z_stream zs[4];
	struct aml_work* zs_worker[4];

	struct rfb_pixel_format dfmt;
	struct rfb_pixel_format sfmt;
	struct nvnc_fb* fb;
	uint64_t pts;

	uint32_t n_rects;
	uint32_t n_jobs;

	struct vec dst;

	tight_done_fn on_frame_done;
	void* userdata;
};

enum tight_tile_state {
	TIGHT_TILE_READY = 0,
	TIGHT_TILE_DAMAGED,
	TIGHT_TILE_ENCODED,
};

struct tight_tile {
	enum tight_tile_state state;
	size_t size;
	uint8_t type;
	char buffer[MAX_TILE_SIZE];
};

struct tight_zs_worker_ctx {
	struct tight_encoder* encoder;
	int index;
};

struct encoder_impl encoder_impl_tight;

static void do_tight_zs_work(void*);
static void on_tight_zs_work_done(void*);
static int schedule_tight_finish(struct tight_encoder* self);

static inline struct tight_encoder* tight_encoder(struct encoder* encoder)
{
	assert(encoder->impl == &encoder_impl_tight);
	return (struct tight_encoder*)encoder;
}

static int tight_encoder_init_stream(z_stream* zs)
{
	int rc = deflateInit2(zs,
	                      /* compression level: */ 1,
	                      /*            method: */ Z_DEFLATED,
	                      /*       window bits: */ 15,
	                      /*         mem level: */ 9,
	                      /*          strategy: */ Z_DEFAULT_STRATEGY);
	return rc == Z_OK ? 0 : -1;
}

static inline struct tight_tile* tight_tile(struct tight_encoder* self,
		uint32_t x, uint32_t y)
{
	return &self->grid[x + y * self->grid_width];
}

static inline uint32_t tight_tile_width(struct tight_encoder* self,
		uint32_t x)
{
	return x + TSL > self->width ? self->width - x : TSL;
}

static inline uint32_t tight_tile_height(struct tight_encoder* self,
		uint32_t y)
{
	return y + TSL > self->height ? self->height - y : TSL;
}

static int tight_init_zs_worker(struct tight_encoder* self, int index)
{
	struct tight_zs_worker_ctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->encoder = self;
	ctx->index = index;

	self->zs_worker[index] =
		aml_work_new(do_tight_zs_work, on_tight_zs_work_done, ctx, free);
	if (!self->zs_worker[index])
		goto failure;

	return 0;

failure:
	free(ctx);
	return -1;
}

static int tight_encoder_resize(struct tight_encoder* self, uint32_t width,
		uint32_t height)
{
	self->width = width;
	self->height = height;

	self->grid_width = UDIV_UP(width, 64);
	self->grid_height = UDIV_UP(height, 64);

	if (self->grid)
		free(self->grid);

	self->grid = calloc(self->grid_width * self->grid_height,
			sizeof(*self->grid));
	return self->grid ? 0 : -1;
}

static int tight_encoder_init(struct tight_encoder* self, uint32_t width,
		uint32_t height)
{
	memset(self, 0, sizeof(*self));
	if (tight_encoder_resize(self, width, height) < 0)
		return -1;

	tight_encoder_init_stream(&self->zs[0]);
	tight_encoder_init_stream(&self->zs[1]);
	tight_encoder_init_stream(&self->zs[2]);
	tight_encoder_init_stream(&self->zs[3]);

	tight_init_zs_worker(self, 0);
	tight_init_zs_worker(self, 1);
	tight_init_zs_worker(self, 2);
	tight_init_zs_worker(self, 3);

	aml_require_workers(aml_get_default(), 1);

	self->pts = NVNC_NO_PTS;

	return 0;
}

static void tight_encoder_destroy(struct tight_encoder* self)
{
	aml_unref(self->zs_worker[3]);
	aml_unref(self->zs_worker[2]);
	aml_unref(self->zs_worker[1]);
	aml_unref(self->zs_worker[0]);

	deflateEnd(&self->zs[3]);
	deflateEnd(&self->zs[2]);
	deflateEnd(&self->zs[1]);
	deflateEnd(&self->zs[0]);

	free(self->grid);
}

static int tight_apply_damage(struct tight_encoder* self,
		struct pixman_region16* damage)
{
	int n_damaged = 0;

	/* Align damage to tile grid */
	for (uint32_t y = 0; y < self->grid_height; ++y)
		for (uint32_t x = 0; x < self->grid_width; ++x) {
			struct pixman_box16 box = {
				.x1 = x * TSL,
				.y1 = y * TSL,
				.x2 = ((x + 1) * TSL) - 1,
				.y2 = ((y + 1) * TSL) - 1,
			};

			pixman_region_overlap_t overlap
				= pixman_region_contains_rectangle(damage, &box);

			if (overlap != PIXMAN_REGION_OUT) {
				++n_damaged;
				tight_tile(self, x, y)->state = TIGHT_TILE_DAMAGED;
			} else {
				tight_tile(self, x, y)->state = TIGHT_TILE_READY;
			}
		}

	return n_damaged;
}

static void tight_encode_size(struct vec* dst, size_t size)
{
	vec_fast_append_8(dst, (size & 0x7f) | ((size >= 128) << 7));
	if (size >= 128)
		vec_fast_append_8(dst, ((size >> 7) & 0x7f) | ((size >= 16384) << 7));
	if (size >= 16384)
		vec_fast_append_8(dst, (size >> 14) & 0xff);
}

static int tight_deflate(struct tight_tile* tile, void* src,
			 size_t len, z_stream* zs, bool flush)
{
	zs->next_in = src;
	zs->avail_in = len;

	do {
		if (tile->size >= MAX_TILE_SIZE)
			return -1;

		zs->next_out = ((Bytef*)tile->buffer) + tile->size;
		zs->avail_out = MAX_TILE_SIZE - tile->size;

		int r = deflate(zs, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
		if (r == Z_STREAM_ERROR)
			return -1;

		tile->size = zs->next_out - (Bytef*)tile->buffer;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}

static void tight_encode_tile_basic(struct tight_encoder* self,
		struct tight_tile* tile, uint32_t x, uint32_t y_start,
		uint32_t width, uint32_t height, int zs_index)
{
	z_stream* zs = &self->zs[zs_index];
	tile->type = TIGHT_BASIC | TIGHT_STREAM(zs_index);

	int bytes_per_cpixel = calc_bytes_per_cpixel(&self->dfmt);
	assert(bytes_per_cpixel <= 4);
	uint8_t row[TSL * 4];

	struct rfb_pixel_format cfmt = { 0 };
	if (bytes_per_cpixel == 3)
		rfb_pixfmt_from_fourcc(&cfmt, DRM_FORMAT_XBGR8888);
	else
		memcpy(&cfmt, &self->dfmt, sizeof(cfmt));

	uint32_t* addr = nvnc_fb_get_addr(self->fb);
	int32_t stride = nvnc_fb_get_stride(self->fb);

	// TODO: Limit width and hight to the sides
	for (uint32_t y = y_start; y < y_start + height; ++y) {
		void* img = addr + x + y * stride;
		pixel32_to_cpixel(row, &cfmt, img, &self->sfmt,
				bytes_per_cpixel, width);

		// TODO What to do if the buffer fills up?
		if (tight_deflate(tile, row, bytes_per_cpixel * width,
				zs, y == y_start + height - 1) < 0)
			abort();
	}

}

#ifdef HAVE_JPEG
static enum TJPF tight_get_jpeg_pixfmt(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		return TJPF_XBGR;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		return TJPF_XRGB;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return TJPF_BGRX;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return TJPF_RGBX;
	}

	return TJPF_UNKNOWN;
}

static int tight_encode_tile_jpeg(struct tight_encoder* self,
		struct tight_tile* tile, uint32_t x, uint32_t y, uint32_t width,
		uint32_t height)
{
	tile->type = TIGHT_JPEG;

	unsigned char* buffer = NULL;
	unsigned long size = 0;

	int quality; /* 1 - 100 */

	switch (self->quality) {
	case TIGHT_QUALITY_HIGH: quality = 66; break;
	case TIGHT_QUALITY_LOW: quality = 33; break;
	default: abort();
	}

	uint32_t fourcc = nvnc_fb_get_fourcc_format(self->fb);
	enum TJPF tjfmt = tight_get_jpeg_pixfmt(fourcc);
	if (tjfmt == TJPF_UNKNOWN)
		return -1;

	tjhandle handle = tjInitCompress();
	if (!handle)
		return -1;

	uint32_t* addr = nvnc_fb_get_addr(self->fb);
	int32_t stride = nvnc_fb_get_stride(self->fb);
	void* img = (uint32_t*)addr + x + y * stride;

	int rc = -1;
	rc = tjCompress2(handle, img, width, stride * 4, height, tjfmt, &buffer,
			&size, TJSAMP_422, quality, TJFLAG_FASTDCT);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode tight JPEG box: %s",
				tjGetErrorStr());
		goto failure;
	}

	if (size > MAX_TILE_SIZE) {
		nvnc_log(NVNC_LOG_ERROR, "Whoops, encoded JPEG was too big for the buffer");
		goto failure;
	}

	memcpy(tile->buffer, buffer, size);
	tile->size = size;

	rc = 0;
	tjFree(buffer);
failure:
	tjDestroy(handle);

	return rc;
}
#endif /* HAVE_JPEG */

static void tight_encode_tile(struct tight_encoder* self,
		uint32_t gx, uint32_t gy)
{
	struct tight_tile* tile = tight_tile(self, gx, gy);

	uint32_t x = gx * TSL;
	uint32_t y = gy * TSL;

	uint32_t width = tight_tile_width(self, x);
	uint32_t height = tight_tile_height(self, y);

	tile->size = 0;

#ifdef HAVE_JPEG
	switch (self->quality) {
	case TIGHT_QUALITY_LOSSLESS:
		tight_encode_tile_basic(self, tile, x, y, width, height, gx % 4);
		break;
	case TIGHT_QUALITY_HIGH:
	case TIGHT_QUALITY_LOW:
		// TODO: Use more workers for jpeg
		tight_encode_tile_jpeg(self, tile, x, y, width, height);
		break;
	case TIGHT_QUALITY_UNSPEC:
		abort();
	}
#else
	tight_encode_tile_basic(self, tile, x, y, width, height, gx % 4);
#endif

	tile->state = TIGHT_TILE_ENCODED;
}

static void do_tight_zs_work(void* obj)
{
	struct tight_zs_worker_ctx* ctx = aml_get_userdata(obj);
	struct tight_encoder* self = ctx->encoder;
	int index = ctx->index;

	for (uint32_t y = 0; y < self->grid_height; ++y)
		for (uint32_t x = index; x < self->grid_width; x += 4)
			if (tight_tile(self, x, y)->state == TIGHT_TILE_DAMAGED)
				tight_encode_tile(self, x, y);
}

static void on_tight_zs_work_done(void* obj)
{
	struct tight_zs_worker_ctx* ctx = aml_get_userdata(obj);
	struct tight_encoder* self = ctx->encoder;

	if (--self->n_jobs == 0) {
		nvnc_fb_unref(self->fb);
		schedule_tight_finish(self);
	}
}

static int tight_schedule_zs_work(struct tight_encoder* self, int index)
{
	int rc = aml_start(aml_get_default(), self->zs_worker[index]);
	if (rc >= 0)
		++self->n_jobs;

	return rc;
}

static int tight_schedule_encoding_jobs(struct tight_encoder* self)
{
	for (int i = 0; i < 4; ++i)
		if (tight_schedule_zs_work(self, i) < 0)
			return -1;

	return 0;
}

static void tight_finish_tile(struct tight_encoder* self,
		uint32_t gx, uint32_t gy)
{
	struct tight_tile* tile = tight_tile(self, gx, gy);

	uint16_t x_pos = self->encoder.x_pos;
	uint16_t y_pos = self->encoder.y_pos;

	uint32_t x = gx * TSL;
	uint32_t y = gy * TSL;

	uint32_t width = tight_tile_width(self, x);
	uint32_t height = tight_tile_height(self, y);

	encode_rect_head(&self->dst, RFB_ENCODING_TIGHT, x_pos + x, y_pos + y,
			width, height);

	vec_append(&self->dst, &tile->type, sizeof(tile->type));
	tight_encode_size(&self->dst, tile->size);
	vec_append(&self->dst, tile->buffer, tile->size);

	tile->state = TIGHT_TILE_READY;
}

static void tight_finish(struct tight_encoder* self)
{
	for (uint32_t y = 0; y < self->grid_height; ++y)
		for (uint32_t x = 0; x < self->grid_width; ++x)
			if (tight_tile(self, x, y)->state == TIGHT_TILE_ENCODED)
				tight_finish_tile(self, x, y);
}

static void do_tight_finish(void* obj)
{
	// TODO: Make sure there's no use-after-free here
	struct tight_encoder* self = aml_get_userdata(obj);
	tight_finish(self);
}

static void on_tight_finished(void* obj)
{
	struct tight_encoder* self = aml_get_userdata(obj);

	struct rcbuf* result = rcbuf_new(self->dst.data, self->dst.len);
	assert(result);

	self->encoder.n_rects = self->n_rects;

	encoder_finish_frame(&self->encoder, result, self->pts);

	self->pts = NVNC_NO_PTS;
	rcbuf_unref(result);
}

static int schedule_tight_finish(struct tight_encoder* self)
{
	struct aml_work* work = aml_work_new(do_tight_finish, on_tight_finished,
			self, NULL);
	if (!work)
		return -1;

	int rc = aml_start(aml_get_default(), work);
	aml_unref(work);
	return rc;
}

struct encoder* tight_encoder_new(uint16_t width, uint16_t height)
{
	struct tight_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	if (tight_encoder_init(self, width, height) < 0) {
		free(self);
		return NULL;
	}

	self->encoder.impl = &encoder_impl_tight;

	return (struct encoder*)self;
}

static void tight_encoder_destroy_wrapper(struct encoder* encoder)
{
	tight_encoder_destroy(tight_encoder(encoder));
	free(encoder);
}

static void tight_encoder_set_output_format(struct encoder* encoder,
		const struct rfb_pixel_format* pixfmt)
{
	struct tight_encoder* self = tight_encoder(encoder);
	memcpy(&self->dfmt, pixfmt, sizeof(self->dfmt));
}

static void tight_encoder_set_quality(struct encoder* encoder, int value)
{
	struct tight_encoder* self = tight_encoder(encoder);
	self->quality = value;
}

static int tight_encoder_resize_wrapper(struct encoder* encoder, uint16_t width,
		uint16_t height)
{
	struct tight_encoder* self = tight_encoder(encoder);
	return tight_encoder_resize(self, width, height);
}

static int tight_encoder_encode(struct encoder* encoder, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	struct tight_encoder* self = tight_encoder(encoder);
	int rc;

	self->encoder.n_rects = 0;

	rc = rfb_pixfmt_from_fourcc(&self->sfmt, nvnc_fb_get_fourcc_format(fb));
	assert(rc == 0);

	self->fb = fb;
	self->pts = nvnc_fb_get_pts(fb);

	rc = nvnc_fb_map(self->fb);
	if (rc < 0)
		return -1;

	uint32_t width = nvnc_fb_get_width(fb);
	uint32_t height = nvnc_fb_get_height(fb);
	rc = vec_init(&self->dst, width * height * 4);
	if (rc < 0)
		return -1;

	self->n_rects = tight_apply_damage(self, damage);
	assert(self->n_rects > 0);

	nvnc_fb_ref(self->fb);

	if (tight_schedule_encoding_jobs(self) < 0) {
		nvnc_fb_unref(self->fb);
		vec_destroy(&self->dst);
		return -1;
	}

	return 0;
}

struct encoder_impl encoder_impl_tight = {
	.destroy = tight_encoder_destroy_wrapper,
	.set_output_format = tight_encoder_set_output_format,
	.set_tight_quality = tight_encoder_set_quality,
	.resize = tight_encoder_resize_wrapper,
	.encode = tight_encoder_encode,
};
