/*
 * Copyright (c) 2021 - 2025 Andri Yngvason
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

#include "compositor.h"
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
#include <pthread.h>

struct fb_side_data {
	struct pixman_region16 buffer_damage; // Stored in logical coordinates
	double scale_x; // Horizontal scale factor (physical/logical)
	double scale_y; // Vertical scale factor (physical/logical)
	LIST_ENTRY(fb_side_data) link;
};

LIST_HEAD(fb_side_data_list, fb_side_data);

struct compositor {
	struct nvnc_fb_pool* pool;
	struct fb_side_data_list fb_side_data_list;
	uint32_t seq_head, seq_tail;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool is_being_destroyed;
};

struct compositor_work {
	struct compositor* compositor;
	struct pixman_region16 frame_damage;
	struct nvnc_composite_fb src;
	struct nvnc_fb* dst;
	uint32_t seq;
	compositor_fn on_done;
	void* userdata;
};

static void fb_side_data_destroy(void* userdata)
{
	struct fb_side_data* fb_side_data = userdata;
	LIST_REMOVE(fb_side_data, link);
	pixman_region_fini(&fb_side_data->buffer_damage);
	free(fb_side_data);
}

static void scale_region(struct pixman_region16* dst,
		const struct pixman_region16* src,
		double scale_x, double scale_y)
{
	int n_rects = 0;
	struct pixman_box16* rects = pixman_region_rectangles(
			(struct pixman_region16*)src, &n_rects);

	pixman_region_init(dst);
	for (int i = 0; i < n_rects; ++i) {
		int x1 = (int)(rects[i].x1 * scale_x);
		int y1 = (int)(rects[i].y1 * scale_y);
		int x2 = (int)(rects[i].x2 * scale_x + 0.5);
		int y2 = (int)(rects[i].y2 * scale_y + 0.5);
		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

static void compositor_damage_all_buffers(struct compositor* self,
		struct pixman_region16* region)
{
	struct fb_side_data *item;
	LIST_FOREACH(item, &self->fb_side_data_list, link)
		pixman_region_union(&item->buffer_damage, &item->buffer_damage,
				region);
}

static void compositor_work_free(void* userdata)
{
	struct compositor_work* work = userdata;

	nvnc_composite_fb_release(&work->src);
	nvnc_composite_fb_unref(&work->src);

	nvnc_fb_unref(work->dst);

	pixman_region_fini(&work->frame_damage);

	free(work);
}

struct compositor* compositor_create(void)
{
	struct compositor* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->pool = nvnc_fb_pool_new(0, 0, DRM_FORMAT_INVALID, 0);
	if (!self->pool) {
		free(self);
		return NULL;
	}

	LIST_INIT(&self->fb_side_data_list);

	pthread_mutex_init(&self->mutex, NULL);
	pthread_cond_init(&self->cond, NULL);

	return self;
}

void compositor_destroy(struct compositor* self)
{
	self->is_being_destroyed = true;

	while (self->seq_tail != self->seq_head) {
		aml_poll(aml_get_default(), -1);
		aml_dispatch(aml_get_default());
	}

	nvnc_fb_pool_unref(self->pool);
	free(self);
}

static void do_work(struct aml_work* work)
{
	struct compositor_work* ctx = aml_get_userdata(work);

	struct nvnc_composite_fb* csrc = &ctx->src;
	struct nvnc_fb* dst = ctx->dst;
	struct fb_side_data* dst_side_data = nvnc_get_userdata(dst);

	assert(dst->transform == NVNC_TRANSFORM_NORMAL);

	bool ok __attribute__((unused));

	pixman_format_code_t dst_fmt = 0;
	ok = fourcc_to_pixman_fmt(&dst_fmt, dst->fourcc_format);
	assert(ok);

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			dst_fmt, dst->width, dst->height, dst->addr,
			nvnc_fb_get_pixel_size(dst) * dst->stride);

	/* Side data contains damage in logical coordinates. Scale it to
	 * physical coordinates for rendering.
	 */
	struct pixman_region16 physical_damage;
	scale_region(&physical_damage, &dst_side_data->buffer_damage,
			dst_side_data->scale_x, dst_side_data->scale_y);
	pixman_image_set_clip_region(dstimg, &physical_damage);

	for (int i = 0; i < csrc->n_fbs; ++i) {
		struct nvnc_fb* src = csrc->fbs[i];
		assert(src);

		pixman_format_code_t src_fmt = 0;
		ok = fourcc_to_pixman_fmt(&src_fmt, src->fourcc_format);
		assert(ok);

		pixman_image_t* srcimg = pixman_image_create_bits_no_clear(
				src_fmt, src->width, src->height, src->addr,
				nvnc_fb_get_pixel_size(src) * src->stride);

		uint32_t transformed_width, transformed_height;
		transformed_width = src->width;
		transformed_height = src->height;
		nvnc_transform_dimensions(src->transform, &transformed_width,
				&transformed_height);

		uint32_t src_width, src_height;
		if (src->logical_width) {
			assert(src->logical_height);
			src_width = src->logical_width;
			src_height = src->logical_height;
		} else {
			src_width = transformed_width;
			src_height = transformed_height;
		}

		double horiz_scale = transformed_width / (double)src_width;
		double vert_scale = transformed_height / (double)src_height;

		pixman_transform_t pxform;
		pixman_transform_init_identity(&pxform);

		pixman_transform_scale(&pxform, NULL,
				pixman_double_to_fixed(horiz_scale),
				pixman_double_to_fixed(vert_scale));

		pixman_transform_t rotation;
		nvnc_transform_to_pixman_transform(&rotation, src->transform,
				src->width, src->height);

		pixman_transform_multiply(&pxform, &rotation, &pxform);

		if (src_width != transformed_width ||
				src_height != transformed_height)
			pixman_image_set_filter(srcimg, PIXMAN_FILTER_BILINEAR,
					NULL, 0);

		pixman_image_set_transform(srcimg, &pxform);

		pixman_image_composite(PIXMAN_OP_SRC, srcimg, NULL, dstimg,
				0, 0,
				0, 0,
				src->x_off, src->y_off,
				src_width, src_height);

		pixman_image_unref(srcimg);
	}

	pixman_region_fini(&physical_damage);
	pixman_image_unref(dstimg);

	// Block the thread until previous jobs have completed
	struct compositor* compositor = ctx->compositor;
	pthread_mutex_lock(&compositor->mutex);
	while (compositor->seq_tail + 1 != ctx->seq)
		pthread_cond_wait(&compositor->cond, &compositor->mutex);
	pthread_mutex_unlock(&compositor->mutex);
}

static void on_work_done(struct aml_work* work)
{
	struct compositor_work* ctx = aml_get_userdata(work);
	struct compositor* compositor = ctx->compositor;

	// Advertise to subsequent tasks that they may continue
	pthread_mutex_lock(&compositor->mutex);
	assert(compositor->seq_tail + 1 == ctx->seq);
	compositor->seq_tail = ctx->seq;
	pthread_cond_broadcast(&compositor->cond);
	pthread_mutex_unlock(&compositor->mutex);

	struct fb_side_data* fb_side_data = nvnc_get_userdata(ctx->dst);
	assert(fb_side_data);
	LIST_INSERT_HEAD(&compositor->fb_side_data_list, fb_side_data, link);

	struct nvnc_composite_fb cfb;
	struct nvnc_fb *fbs[] = { ctx->dst, NULL };
	nvnc_composite_fb_init(&cfb, fbs);

	if (!compositor->is_being_destroyed)
		ctx->on_done(&cfb, &ctx->frame_damage, ctx->userdata);

	nvnc_composite_fb_release(&cfb);
}

static bool is_compositing_needed(const struct nvnc_composite_fb* cfb)
{
	double first_scale_x = 0.0, first_scale_y = 0.0;
	bool has_scaling = false;

	for (int i = 0; i < cfb->n_fbs; ++i) {
		struct nvnc_fb* fb = cfb->fbs[i];
		assert(fb);

		// encoders don't know how to transform
		if (fb->transform != NVNC_TRANSFORM_NORMAL)
			return true;

		// Calculate the scaling factor for this buffer
		double scale_x = 1.0, scale_y = 1.0;
		if (fb->logical_width && fb->logical_width != fb->width) {
			scale_x = (double)fb->width / fb->logical_width;
			has_scaling = true;
		}
		if (fb->logical_height && fb->logical_height != fb->height) {
			scale_y = (double)fb->height / fb->logical_height;
			has_scaling = true;
		}

		// Check if all buffers have the same scaling factor
		if (i == 0) {
			first_scale_x = scale_x;
			first_scale_y = scale_y;
		} else if (has_scaling) {
			// If scaling factors differ, compositing is needed
			if (scale_x != first_scale_x || scale_y != first_scale_y)
				return true;
		}
	}

	// encoders can composite buffers without scaling/transform, or with
	// equal scaling across all buffers
	return false;
}

int compositor_feed(struct compositor* self, struct nvnc_composite_fb* cfb,
		struct pixman_region16* damage, compositor_fn on_done,
		void* userdata)
{
	DTRACE_PROBE2(neatvnc, compositor_feed, self, fb->pts);

	nvnc_assert(cfb->n_fbs != 0, "Composite fb contains no fbs");

	if (self->seq_tail == self->seq_head && !is_compositing_needed(cfb)) {
		on_done(cfb, damage, userdata);
		return 0;
	}

	uint32_t width = nvnc_composite_fb_width(cfb);
	uint32_t height = nvnc_composite_fb_height(cfb);

	struct nvnc_fb* first_fb = cfb->fbs[0];
	assert(first_fb);

	nvnc_fb_pool_resize(self->pool, width, height, first_fb->fourcc_format,
			width);

	struct aml* aml = aml_get_default();
	assert(aml);

	struct compositor_work* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	pixman_region_init(&ctx->frame_damage);
	pixman_region_copy(&ctx->frame_damage, damage);

	ctx->dst = nvnc_fb_pool_acquire(self->pool);
	if (!ctx->dst)
		goto acquire_failure;

	struct fb_side_data* fb_side_data = nvnc_get_userdata(ctx->dst);
	if (!fb_side_data) {
		fb_side_data = calloc(1, sizeof(*fb_side_data));
		if (!fb_side_data)
			goto side_data_failure;

		/* This is a new buffer, so the whole surface is damaged.
		 * Damage is stored in logical coordinates (= physical coords
		 * of destination buffer).
		 */
		pixman_region_init_rect(&fb_side_data->buffer_damage, 0, 0,
				width, height);
		
		/* Destination buffer is in logical coordinate space, so scale
		 * factor is 1.0 (no scaling needed when rendering).
		 */
		fb_side_data->scale_x = 1.0;
		fb_side_data->scale_y = 1.0;

		nvnc_set_userdata(ctx->dst, fb_side_data, fb_side_data_destroy);
		LIST_INSERT_HEAD(&self->fb_side_data_list, fb_side_data, link);
	}

	compositor_damage_all_buffers(self, damage);

	nvnc_fb_hold(ctx->dst);

	// The side data entry is removed from the list when damaging is done
	// and added back when the job is finished.
	LIST_REMOVE(fb_side_data, link);

	nvnc_composite_fb_copy(&ctx->src, cfb);
	nvnc_composite_fb_hold(&ctx->src);

	ctx->compositor = self;
	ctx->on_done = on_done;
	ctx->userdata = userdata;
	ctx->seq = ++self->seq_head;

	struct aml_work* work = aml_work_new(do_work, on_work_done, ctx,
			compositor_work_free);
	if (!work) {
		compositor_work_free(ctx);
		return -1;
	}

	nvnc_composite_fb_map(&ctx->src);

	int rc = aml_start(aml, work);
	aml_unref(work);
	return rc;

side_data_failure:
	nvnc_fb_pool_release(self->pool, ctx->dst);
acquire_failure:
	free(ctx);
	return -1;
}
