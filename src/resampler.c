/*
 * Copyright (c) 2021 Andri Yngvason
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

#include <stdlib.h>
#include <aml.h>
#include <pixman.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

struct resampler {
	struct nvnc_fb_pool *pool;
};

struct resampler_work {
	struct pixman_region16 damage;
	struct nvnc_fb* src;
	struct nvnc_fb* dst;
	resampler_fn on_done;
	void* userdata;
};

static void resampler_work_free(void* userdata)
{
	struct resampler_work* work = userdata;

	nvnc_fb_release(work->src);
	nvnc_fb_unref(work->src);

	nvnc_fb_unref(work->dst);

	pixman_region_fini(&work->damage);

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

	return self;
}

void resampler_destroy(struct resampler* self)
{
	nvnc_fb_pool_unref(self->pool);
	free(self);
}

static void do_work(void* handle)
{
	struct aml_work* work = handle;
	struct resampler_work* ctx = aml_get_userdata(work);

	struct nvnc_fb* src = ctx->src;
	struct nvnc_fb* dst = ctx->dst;

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

	pixman_image_set_clip_region(dstimg, &ctx->damage);

	pixman_image_composite(PIXMAN_OP_OVER, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			dst->width, dst->height);

	pixman_image_unref(srcimg);
	pixman_image_unref(dstimg);
}

static void on_work_done(void* handle)
{
	struct aml_work* work = handle;
	struct resampler_work* ctx = aml_get_userdata(work);

	ctx->on_done(ctx->dst, &ctx->damage, ctx->userdata);
}

int resampler_feed(struct resampler* self, struct nvnc_fb* fb,
		struct pixman_region16* damage, resampler_fn on_done,
		void* userdata)
{
	if (fb->transform == NVNC_TRANSFORM_NORMAL) {
		on_done(fb, damage, userdata);
		return 0;
	}

	uint32_t width = fb->width;
	uint32_t height = fb->height;

	nvnc_transform_dimensions(fb->transform, &width, &height);
	nvnc_fb_pool_resize(self->pool, width, height, fb->fourcc_format,
			fb->stride);

	struct aml* aml = aml_get_default();
	assert(aml);

	struct resampler_work* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	pixman_region_init(&ctx->damage);
	pixman_region_copy(&ctx->damage, damage);

	ctx->dst = nvnc_fb_pool_acquire(self->pool);

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
}