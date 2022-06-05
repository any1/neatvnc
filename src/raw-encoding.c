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

#include <stdlib.h>
#include <pixman.h>
#include <aml.h>

struct encoder* raw_encoder_new(void);

struct raw_encoder {
	struct encoder encoder;

	struct rfb_pixel_format output_format;

	struct nvnc_fb* current_fb;
	struct pixman_region16 current_damage;

	struct rcbuf *current_result;

	struct aml_work* work;
};

struct encoder_impl encoder_impl_raw;

static inline struct raw_encoder* raw_encoder(struct encoder* encoder)
{
	assert(encoder->impl == &encoder_impl_raw);
	return (struct raw_encoder*)encoder;
}

static int raw_encode_box(struct raw_encoder* self, struct vec* dst,
                          const struct rfb_pixel_format* dst_fmt,
                          const struct nvnc_fb* fb,
                          const struct rfb_pixel_format* src_fmt, int x_start,
                          int y_start, int stride, int width, int height)
{
	uint16_t x_pos = self->encoder.x_pos;
	uint16_t y_pos = self->encoder.y_pos;

	int rc = -1;

	rc = encode_rect_head(dst, RFB_ENCODING_RAW, x_pos + x_start,
			y_pos + y_start, width, height);
	if (rc < 0)
		return -1;

	uint32_t* b = fb->addr;

	int bpp = dst_fmt->bits_per_pixel / 8;

	rc = vec_reserve(dst, width * height * bpp + dst->len);
	if (rc < 0)
		return -1;

	uint8_t* d = dst->data;

	for (int y = y_start; y < y_start + height; ++y) {
		pixel32_to_cpixel(d + dst->len, dst_fmt,
		                  b + x_start + y * stride, src_fmt,
		                  bpp, width);
		dst->len += width * bpp;
	}

	return 0;
}

static int raw_encode_frame(struct raw_encoder* self, struct vec* dst,
		const struct rfb_pixel_format* dst_fmt, struct nvnc_fb* src,
		const struct rfb_pixel_format* src_fmt,
		struct pixman_region16* region)
{
	int rc = -1;

	self->encoder.n_rects = 0;

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(region, &n_rects);
	if (n_rects > UINT16_MAX) {
		box = pixman_region_extents(region);
		n_rects = 1;
	}

	rc = nvnc_fb_map(src);
	if (rc < 0)
		return -1;

	rc = vec_reserve(dst, src->width * src->height * 4);
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = raw_encode_box(self, dst, dst_fmt, src, src_fmt, x, y,
				    src->stride, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	self->encoder.n_rects = n_rects;
	return 0;
}

static void raw_encoder_do_work(void* obj)
{
	struct raw_encoder* self = aml_get_userdata(obj);
	int rc;

	struct nvnc_fb* fb = self->current_fb;
	assert(fb);

	// TODO: Calculate the ideal buffer size based on the size of the
	// damaged area.
	size_t buffer_size = nvnc_fb_get_stride(fb) * nvnc_fb_get_height(fb) *
		nvnc_fb_get_pixel_size(fb);

	struct vec dst;
	rc = vec_init(&dst, buffer_size);
	assert(rc == 0);

	struct rfb_pixel_format src_fmt;
	rc = rfb_pixfmt_from_fourcc(&src_fmt, nvnc_fb_get_fourcc_format(fb));
	assert(rc == 0);

	rc = raw_encode_frame(self, &dst, &self->output_format, fb, &src_fmt,
			&self->current_damage);
	assert(rc == 0);

	self->current_result = rcbuf_new(dst.data, dst.len);
	assert(self->current_result);
}

static void raw_encoder_on_done(void* obj)
{
	struct raw_encoder* self = aml_get_userdata(obj);

	assert(self->current_result);

	uint64_t pts = nvnc_fb_get_pts(self->current_fb);
	nvnc_fb_unref(self->current_fb);
	self->current_fb = NULL;

	pixman_region_clear(&self->current_damage);

	struct rcbuf* result = self->current_result;
	self->current_result = NULL;

	aml_unref(self->work);
	self->work = NULL;

	encoder_finish_frame(&self->encoder, result, pts);

	rcbuf_unref(result);
}

struct encoder* raw_encoder_new(void)
{
	struct raw_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->encoder.impl = &encoder_impl_raw;

	pixman_region_init(&self->current_damage);

	return (struct encoder*)self;
}

static void raw_encoder_destroy(struct encoder* encoder)
{
	struct raw_encoder* self = raw_encoder(encoder);
	pixman_region_fini(&self->current_damage);
	if (self->work)
		aml_unref(self->work);
	free(self);
}

static void raw_encoder_set_output_format(struct encoder* encoder,
		const struct rfb_pixel_format* pixfmt)
{
	struct raw_encoder* self = raw_encoder(encoder);
	memcpy(&self->output_format, pixfmt, sizeof(self->output_format));
}

static int raw_encoder_encode(struct encoder* encoder, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	struct raw_encoder* self = raw_encoder(encoder);

	assert(!self->current_fb);

	self->work = aml_work_new(raw_encoder_do_work, raw_encoder_on_done,
			self, NULL);
	if (!self->work)
		return -1;

	self->current_fb = fb;
	nvnc_fb_ref(self->current_fb);
	pixman_region_copy(&self->current_damage, damage);

	int rc = aml_start(aml_get_default(), self->work);
	if (rc < 0) {
		aml_unref(self->work);
		self->work = NULL;
		pixman_region_clear(&self->current_damage);
		nvnc_fb_unref(self->current_fb);
		self->current_fb = NULL;
	}

	return rc;
}

struct encoder_impl encoder_impl_raw = {
	.destroy = raw_encoder_destroy,
	.set_output_format = raw_encoder_set_output_format,
	.encode = raw_encoder_encode,
};
