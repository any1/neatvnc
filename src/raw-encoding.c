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
#include "vec.h"
#include "fb.h"
#include "pixels.h"
#include "enc-util.h"
#include "encoder.h"
#include "rcbuf.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <pixman.h>
#include <aml.h>

struct encoder* raw_encoder_new(void);

struct raw_encoder {
	struct encoder encoder;
	struct rfb_pixel_format output_format;
	struct aml_work* work;
};

struct raw_encoder_work {
	struct raw_encoder* parent;
	struct rfb_pixel_format output_format;
	struct nvnc_fb* fb;
	struct pixman_region16 damage;
	int n_rects;
	uint16_t x_pos, y_pos;
	struct rcbuf *result;
};

struct encoder_impl encoder_impl_raw;

static inline struct raw_encoder* raw_encoder(struct encoder* encoder)
{
	assert(encoder->impl == &encoder_impl_raw);
	return (struct raw_encoder*)encoder;
}

static int raw_encode_box(struct raw_encoder_work* ctx, struct vec* dst,
                          const struct rfb_pixel_format* dst_fmt,
                          const struct nvnc_fb* fb,
                          const struct rfb_pixel_format* src_fmt, int x_start,
                          int y_start, int stride, int width, int height)
{
	uint16_t x_pos = ctx->x_pos;
	uint16_t y_pos = ctx->y_pos;

	int rc = -1;

	rc = encode_rect_head(dst, RFB_ENCODING_RAW, x_pos + x_start,
			y_pos + y_start, width, height);
	if (rc < 0)
		return -1;

	int bpp = dst_fmt->bits_per_pixel / 8;
	int src_bpp = src_fmt->bits_per_pixel / 8;

	rc = vec_reserve(dst, width * height * bpp + dst->len);
	if (rc < 0)
		return -1;

    if (src_bpp == 3) {
        uint32_t fb_width = nvnc_fb_get_width(fb);
        stride = fb_width * src_bpp;
    }

	uint8_t* d = dst->data;

	for (int y = y_start; y < y_start + height; ++y) {

        if (src_bpp == 4) {
			uint32_t* b = fb->addr;
			//each x is 4 bytes
		    pixel32_to_cpixel(d + dst->len, dst_fmt,
		                  b + x_start + y * stride, src_fmt,
		                  bpp, width);
		} else if (src_bpp == 3) {
            uint8_t* b = fb->addr;
			//each x is 3 bytes

            pixel24_to_cpixel(d + dst->len, dst_fmt,
		                  b + (x_start * 3) + (y * stride), src_fmt,
		                  bpp, width);
		}
		dst->len += width * bpp;
	}

	return 0;
}

static int raw_encode_frame(struct raw_encoder_work* ctx, struct vec* dst,
		const struct rfb_pixel_format* dst_fmt, struct nvnc_fb* src,
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

	rc = nvnc_fb_map(src);
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = raw_encode_box(ctx, dst, dst_fmt, src, src_fmt, x, y,
				    src->stride, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	ctx->n_rects = n_rects;
	return 0;
}

static void raw_encoder_do_work(void* obj)
{
	struct raw_encoder_work* ctx = aml_get_userdata(obj);
	int rc;

	struct nvnc_fb* fb = ctx->fb;
	assert(fb);

	//frame buffer and dest buffer could be different sizes, so lets just be certain and calculate off dest format
	size_t bpp = ctx->output_format.bits_per_pixel / 8;
	size_t n_rects = pixman_region_n_rects(&ctx->damage);
	if (n_rects > UINT16_MAX)
		n_rects = 1;
	size_t buffer_size = calculate_region_area(&ctx->damage) * bpp
		+ n_rects * sizeof(struct rfb_server_fb_rect);

	struct vec dst;
	rc = vec_init(&dst, buffer_size);
	assert(rc == 0);

	struct rfb_pixel_format src_fmt;
	rc = rfb_pixfmt_from_fourcc(&src_fmt, nvnc_fb_get_fourcc_format(fb));
	assert(rc == 0);

	rc = raw_encode_frame(ctx, &dst, &ctx->output_format, fb, &src_fmt,
			&ctx->damage);
	assert(rc == 0);

	ctx->result = rcbuf_new(dst.data, dst.len);
	assert(ctx->result);
}

static void raw_encoder_on_done(void* obj)
{
	struct raw_encoder_work* ctx = aml_get_userdata(obj);
	struct raw_encoder* self = ctx->parent;

	assert(ctx->result);

	self->encoder.n_rects = ctx->n_rects;

	aml_unref(self->work);
	self->work = NULL;

	uint64_t pts = nvnc_fb_get_pts(ctx->fb);
	encoder_finish_frame(&self->encoder, ctx->result, pts);
}

struct encoder* raw_encoder_new(void)
{
	struct raw_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	encoder_init(&self->encoder, &encoder_impl_raw);

	return (struct encoder*)self;
}

static void raw_encoder_destroy(struct encoder* encoder)
{
	struct raw_encoder* self = raw_encoder(encoder);
	if (self->work) {
		aml_stop(aml_get_default(), self->work);
		aml_unref(self->work);
	}
	free(self);
}

static void raw_encoder_set_output_format(struct encoder* encoder,
		const struct rfb_pixel_format* pixfmt)
{
	struct raw_encoder* self = raw_encoder(encoder);
	memcpy(&self->output_format, pixfmt, sizeof(self->output_format));
}

static void raw_encoder_work_destroy(void* obj)
{
	struct raw_encoder_work* ctx = obj;
	nvnc_fb_unref(ctx->fb);
	pixman_region_fini(&ctx->damage);
	if (ctx->result)
		rcbuf_unref(ctx->result);
	free(ctx);
}

static int raw_encoder_encode(struct encoder* encoder, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	struct raw_encoder* self = raw_encoder(encoder);

	struct raw_encoder_work* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	self->work = aml_work_new(raw_encoder_do_work, raw_encoder_on_done,
			ctx, raw_encoder_work_destroy);
	if (!self->work) {
		free(ctx);
		return -1;
	}

	ctx->parent = self;
	ctx->fb = fb;
	memcpy(&ctx->output_format, &self->output_format,
			sizeof(ctx->output_format));
	ctx->x_pos = self->encoder.x_pos;
	ctx->y_pos = self->encoder.y_pos;
	nvnc_fb_ref(ctx->fb);
	pixman_region_copy(&ctx->damage, damage);

	int rc = aml_start(aml_get_default(), self->work);
	if (rc < 0) {
		aml_unref(self->work);
		self->work = NULL;
	}

	return rc;
}

struct encoder_impl encoder_impl_raw = {
	.destroy = raw_encoder_destroy,
	.set_output_format = raw_encoder_set_output_format,
	.encode = raw_encoder_encode,
};
