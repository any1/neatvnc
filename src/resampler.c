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

struct resampler_work {
	struct pixman_region16 damage;
	struct nvnc_fb* src;
	struct nvnc_fb* dst;
	struct resampler* resampler;
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

int resampler_init(struct resampler* self)
{
	self->pool = nvnc_fb_pool_new(0, 0, DRM_FORMAT_INVALID, 0);
	return self->pool ? 0 : -1;
}

void resampler_destroy(struct resampler* self)
{
	nvnc_fb_pool_unref(self->pool);
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

	ctx->resampler->on_done(ctx->resampler, ctx->dst, &ctx->damage);
}

int resampler_feed(struct resampler* self, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	if (nvnc_fb_get_transform(fb) == NVNC_TRANSFORM_NORMAL) {
		self->on_done(self, fb, damage);
		return 0;
	}

	nvnc_fb_pool_resize(self->pool, fb->width, fb->height,
				fb->fourcc_format, fb->stride);

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

	ctx->resampler = self;

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
