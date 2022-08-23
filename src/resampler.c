/*
 * Copyright (c) 2021 - 2022 Andri Yngvason
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

#include "resampler.h"
#include "neatvnc.h"
#include "fb.h"
#include "transform-util.h"
#include "pixels.h"
#include "usdt.h"

#include <stdlib.h>
#include <aml.h>
#include <pixman.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

struct fb_side_data {
	struct pixman_region16 buffer_damage;
	LIST_ENTRY(fb_side_data) link;
};

LIST_HEAD(fb_side_data_list, fb_side_data);

struct resampler {
	struct nvnc_fb_pool *pool;
	struct fb_side_data_list fb_side_data_list;
};

struct resampler_work {
	struct pixman_region16 frame_damage;
	struct nvnc_fb* src;
	struct nvnc_fb* dst;
	resampler_fn on_done;
	void* userdata;
};

static void fb_side_data_destroy(void* userdata)
{
	struct fb_side_data* fb_side_data = userdata;
	LIST_REMOVE(fb_side_data, link);
	pixman_region_fini(&fb_side_data->buffer_damage);
	free(fb_side_data);
}

static void resampler_damage_all_buffers(struct resampler* self,
		struct pixman_region16* region)
{
	struct fb_side_data *item;
	LIST_FOREACH(item, &self->fb_side_data_list, link)
		pixman_region_union(&item->buffer_damage, &item->buffer_damage,
				region);
}

static void resampler_work_free(void* userdata)
{
	struct resampler_work* work = userdata;

	nvnc_fb_release(work->src);
	nvnc_fb_unref(work->src);

	nvnc_fb_unref(work->dst);

	pixman_region_fini(&work->frame_damage);

	free(work);
}

struct resampler* resampler_create(void)
{
	struct resampler* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->pool = nvnc_fb_pool_new(0, 0, DRM_FORMAT_INVALID, 0);
	if (!self->pool) {
		free(self);
		return NULL;
	}

	LIST_INIT(&self->fb_side_data_list);

	return self;
}

void resampler_destroy(struct resampler* self)
{
	nvnc_fb_pool_unref(self->pool);
	free(self);
}

void resample_now(struct nvnc_fb* dst, struct nvnc_fb* src,
		struct pixman_region16* damage)
{
	assert(dst->transform == NVNC_TRANSFORM_NORMAL);

	bool ok __attribute__((unused));

	pixman_format_code_t dst_fmt = 0;
	ok = fourcc_to_pixman_fmt(&dst_fmt, dst->fourcc_format);
	assert(ok);

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			dst_fmt, dst->width, dst->height, dst->addr,
			nvnc_fb_get_pixel_size(dst) * dst->stride);

	pixman_format_code_t src_fmt = 0;
	ok = fourcc_to_pixman_fmt(&src_fmt, src->fourcc_format);
	assert(ok);

	pixman_image_t* srcimg = pixman_image_create_bits_no_clear(
			src_fmt, src->width, src->height, src->addr,
			nvnc_fb_get_pixel_size(src) * src->stride);

	pixman_transform_t pxform;
	nvnc_transform_to_pixman_transform(&pxform, src->transform,
			src->width, src->height);

	pixman_image_set_transform(srcimg, &pxform);

	/* Side data contains the union of the buffer damage and the frame
	 * damage.
	 */
	if (damage) {
		pixman_image_set_clip_region(dstimg, damage);
	}

	pixman_image_composite(PIXMAN_OP_OVER, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			dst->width, dst->height);

	pixman_image_unref(srcimg);
	pixman_image_unref(dstimg);
}

static void do_work(void* handle)
{
	struct aml_work* work = handle;
	struct resampler_work* ctx = aml_get_userdata(work);

	struct nvnc_fb* src = ctx->src;
	struct nvnc_fb* dst = ctx->dst;
	struct fb_side_data* dst_side_data = nvnc_get_userdata(dst);

	resample_now(dst, src, &dst_side_data->buffer_damage);
}

static void on_work_done(void* handle)
{
	struct aml_work* work = handle;
	struct resampler_work* ctx = aml_get_userdata(work);

	ctx->on_done(ctx->dst, &ctx->frame_damage, ctx->userdata);
}

int resampler_feed(struct resampler* self, struct nvnc_fb* fb,
		struct pixman_region16* damage, resampler_fn on_done,
		void* userdata)
{
	DTRACE_PROBE2(neatvnc, resampler_feed, self, fb->pts);

	if (fb->transform == NVNC_TRANSFORM_NORMAL) {
		on_done(fb, damage, userdata);
		return 0;
	}

	uint32_t width = fb->width;
	uint32_t height = fb->height;

	nvnc_transform_dimensions(fb->transform, &width, &height);
	nvnc_fb_pool_resize(self->pool, width, height, fb->fourcc_format,
			width);

	struct aml* aml = aml_get_default();
	assert(aml);

	struct resampler_work* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	pixman_region_init(&ctx->frame_damage);
	pixman_region_copy(&ctx->frame_damage, damage);

	ctx->dst = nvnc_fb_pool_acquire(self->pool);
	if (!ctx->dst)
		goto acquire_failure;

	struct fb_side_data* fb_side_data = nvnc_get_userdata(fb);
	if (!fb_side_data) {
		fb_side_data = calloc(1, sizeof(*fb_side_data));
		if (!fb_side_data)
			goto side_data_failure;

		/* This is a new buffer, so the whole surface is damaged. */
		pixman_region_init_rect(&fb_side_data->buffer_damage, 0, 0,
				width, height);

		nvnc_set_userdata(fb, fb_side_data, fb_side_data_destroy);
		LIST_INSERT_HEAD(&self->fb_side_data_list, fb_side_data, link);
	}

	resampler_damage_all_buffers(self, damage);

	ctx->src = fb;
	nvnc_fb_ref(fb);
	nvnc_fb_hold(fb);

	ctx->on_done = on_done;
	ctx->userdata = userdata;

	struct aml_work* work = aml_work_new(do_work, on_work_done, ctx,
			resampler_work_free);
	if (!work) {
		resampler_work_free(ctx);
		return -1;
	}

	nvnc_fb_map(fb);

	int rc = aml_start(aml, work);
	aml_unref(work);
	return rc;

side_data_failure:
	nvnc_fb_pool_release(self->pool, ctx->dst);
acquire_failure:
	free(ctx);
	return -1;
}
