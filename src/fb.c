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

#include "fb.h"
#include "pixels.h"
#include "neatvnc.h"
#include "transform-util.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdatomic.h>

#include "config.h"

#ifdef HAVE_GBM
#include <gbm.h>
#endif

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_UP(n, a) (UDIV_UP(n, a) * a)
#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_fb* nvnc_fb_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride)
{
	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	uint32_t bpp = pixel_size_from_fourcc(fourcc_format);

	fb->type = NVNC_FB_SIMPLE;
	fb->ref = 1;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->stride = stride;
	fb->pts = NVNC_NO_PTS;

	size_t size = height * stride * bpp;
	size_t alignment = MAX(4, sizeof(void*));
	size_t aligned_size = ALIGN_UP(size, alignment);

	fb->addr = aligned_alloc(alignment, aligned_size);
	if (!fb->addr) {
		free(fb);
		fb = NULL;
	}

	return fb;
}

EXPORT
struct nvnc_fb* nvnc_fb_from_buffer(void* buffer, uint16_t width, uint16_t height,
		uint32_t fourcc_format, int32_t stride)
{
	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->type = NVNC_FB_SIMPLE;
	fb->ref = 1;
	fb->addr = buffer;
	fb->is_external = true;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->stride = stride;
	fb->pts = NVNC_NO_PTS;

	return fb;
}

EXPORT
struct nvnc_fb* nvnc_fb_from_gbm_bo(struct gbm_bo* bo)
{
#ifdef HAVE_GBM
	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->type = NVNC_FB_GBM_BO;
	fb->ref = 1;
	fb->is_external = true;
	fb->width = gbm_bo_get_width(bo);
	fb->height = gbm_bo_get_height(bo);
	fb->fourcc_format = gbm_bo_get_format(bo);
	fb->bo = bo;
	fb->pts = NVNC_NO_PTS;

	return fb;
#else
	nvnc_log(NVNC_LOG_ERROR, "nvnc_fb_from_gbm_bo was not enabled during build time");
	return NULL;
#endif
}

EXPORT
void* nvnc_fb_get_addr(const struct nvnc_fb* fb)
{
	return fb->addr;
}

EXPORT
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb)
{
	return fb->width;
}

EXPORT
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb)
{
	return fb->height;
}

EXPORT
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb)
{
	return fb->fourcc_format;
}

EXPORT
int32_t nvnc_fb_get_stride(const struct nvnc_fb* fb)
{
	return fb->stride;
}

EXPORT
int nvnc_fb_get_pixel_size(const struct nvnc_fb* fb)
{
	return pixel_size_from_fourcc(fb->fourcc_format);
}

EXPORT
struct gbm_bo* nvnc_fb_get_gbm_bo(const struct nvnc_fb* fb)
{
	return fb->bo;
}

EXPORT
enum nvnc_transform nvnc_fb_get_transform(const struct nvnc_fb* fb)
{
	return fb->transform;
}

EXPORT
enum nvnc_fb_type nvnc_fb_get_type(const struct nvnc_fb* fb)
{
	return fb->type;
}

EXPORT
uint64_t nvnc_fb_get_pts(const struct nvnc_fb* fb)
{
	return fb->pts;
}

static void nvnc__fb_free(struct nvnc_fb* fb)
{
	nvnc_cleanup_fn cleanup = fb->common.cleanup_fn;
	if (cleanup)
		cleanup(fb->common.userdata);

	nvnc_fb_unmap(fb);

	if (!fb->is_external)
		switch (fb->type) {
		case NVNC_FB_UNSPEC:
			abort();
		case NVNC_FB_SIMPLE:
			free(fb->addr);
			break;
		case NVNC_FB_GBM_BO:
#ifdef HAVE_GBM
			gbm_bo_destroy(fb->bo);
#else
			abort();
#endif
			break;
		}

	free(fb);
}

EXPORT
void nvnc_fb_ref(struct nvnc_fb* fb)
{
	fb->ref++;
}

EXPORT
void nvnc_fb_unref(struct nvnc_fb* fb)
{
	if (fb && --fb->ref == 0)
		nvnc__fb_free(fb);
}

EXPORT
void nvnc_fb_set_release_fn(struct nvnc_fb* fb, nvnc_fb_release_fn fn, void* context)
{
	fb->on_release = fn;
	fb->release_context = context;
}

EXPORT
void nvnc_fb_set_transform(struct nvnc_fb* fb, enum nvnc_transform transform)
{
	fb->transform = transform;
}

EXPORT
void nvnc_fb_set_pts(struct nvnc_fb* fb, uint64_t pts)
{
	fb->pts = pts;
}

void nvnc_fb_hold(struct nvnc_fb* fb)
{
	fb->hold_count++;
}

void nvnc_fb_release(struct nvnc_fb* fb)
{
	if (!fb)
		return;

	if (--fb->hold_count != 0)
		return;

	nvnc_fb_unmap(fb);
	fb->pts = NVNC_NO_PTS;

	if (fb->on_release)
		fb->on_release(fb, fb->release_context);
}

int nvnc_fb_map(struct nvnc_fb* fb)
{
#ifdef HAVE_GBM
	if (fb->type != NVNC_FB_GBM_BO || fb->bo_map_handle)
		return 0;

	uint32_t stride = 0;
	fb->addr = gbm_bo_map(fb->bo, 0, 0, fb->width, fb->height,
			GBM_BO_TRANSFER_READ, &stride, &fb->bo_map_handle);
	fb->stride = stride / nvnc_fb_get_pixel_size(fb);
	if (fb->addr)
		return 0;

	fb->bo_map_handle = NULL;
	return -1;
#else
	return 0;
#endif
}

void nvnc_fb_unmap(struct nvnc_fb* fb)
{
#ifdef HAVE_GBM
	if (fb->type != NVNC_FB_GBM_BO)
		return;

	if (fb->bo_map_handle)
		gbm_bo_unmap(fb->bo, fb->bo_map_handle);

	fb->bo_map_handle = NULL;
	fb->addr = NULL;
	fb->stride = 0;
#endif
}

void nvnc_composite_fb_init(struct nvnc_composite_fb* self,
		struct nvnc_fb* fbs[])
{
	int i;
	for (i = 0; fbs[i]; ++i) {
		self->fbs[i] = fbs[i];
	}
	self->n_fbs = i;
}

void nvnc_composite_fb_ref(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);
		nvnc_fb_ref(fb);
	}
}

void nvnc_composite_fb_unref(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);
		nvnc_fb_unref(fb);
	}
}

void nvnc_composite_fb_hold(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);
		nvnc_fb_hold(fb);
	}
}

void nvnc_composite_fb_release(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);
		nvnc_fb_release(fb);
	}
}

int nvnc_composite_fb_map(struct nvnc_composite_fb* self)
{
	int rc = 0;
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);
		if (nvnc_fb_map(fb) < 0)
			rc = 1;
	}
	return rc;
}

void nvnc_composite_fb_copy(struct nvnc_composite_fb* dst,
		const struct nvnc_composite_fb* src)
{
	memcpy(dst, src, sizeof(*dst));
	nvnc_composite_fb_ref(dst);
}

static void nvnc_composite_fb_dimensions(const struct nvnc_composite_fb* self,
		uint16_t* width_out, uint16_t* height_out)
{
	uint16_t width = 0;
	uint16_t height = 0;
	for (int i = 0; i < self->n_fbs; ++i) {
		const struct nvnc_fb* fb = self->fbs[i];
		uint32_t fb_width;
		uint32_t fb_height;
		if (fb->logical_width) {
			assert(fb->logical_height);
			fb_width = fb->logical_width;
			fb_height = fb->logical_height;
		} else {
			fb_width = fb->width;
			fb_height = fb->height;
			nvnc_transform_dimensions(fb->transform, &fb_width,
					&fb_height);
		}
		if (width < fb->x_off + fb_width)
			width = fb->x_off + fb_width;
		if (height < fb->y_off + fb_height)
			height = fb->y_off + fb_height;
	}
	if (width_out)
		*width_out = width;
	if (height_out)
		*height_out = height;
}

uint16_t nvnc_composite_fb_width(const struct nvnc_composite_fb* self)
{
	uint16_t width = 0;
	nvnc_composite_fb_dimensions(self, &width, NULL);
	return width;
}

uint16_t nvnc_composite_fb_height(const struct nvnc_composite_fb* self)
{
	uint16_t height = 0;
	nvnc_composite_fb_dimensions(self, NULL, &height);
	return height;
}

uint64_t nvnc_composite_fb_pts(const struct nvnc_composite_fb* self)
{
	// TODO: Rethink this
	assert(self->n_fbs > 0);
	return self->fbs[0]->pts;
}


static bool nvnc_fbs_overlap(const struct nvnc_fb* a, const struct nvnc_fb* b)
{
	int a_x0 = a->x_off;
	int a_x1 = a->x_off + a->width;
	int a_y0 = a->y_off;
	int a_y1 = a->y_off + a->height;
	int b_x0 = b->x_off;
	int b_x1 = b->x_off + b->width;
	int b_y0 = b->y_off;
	int b_y1 = b->y_off + b->height;
	return a_x0 < b_x1 && a_x1 > b_x0 && a_y0 > b_y1 && a_y1 < b_y0;
}

static bool nvnc_composite_fb_starts_at_zero(
		const struct nvnc_composite_fb* self)
{
	int x_min = INT_MAX;
	int y_min = INT_MAX;

	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* fb = self->fbs[i];
		assert(fb);

		x_min = MIN(x_min, (int)fb->x_off);
		y_min = MIN(y_min, (int)fb->y_off);
	}

	return x_min == 0 && y_min == 0;
}

static bool nvnc_composite_fb_contains_overlaps(
		const struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_fb* a = self->fbs[i];
		assert(a);

		for (int j = i + 1; j < self->n_fbs; ++j) {
			struct nvnc_fb* b = self->fbs[j];
			assert(b);

			return nvnc_fbs_overlap(a, b);
		}
	}

	return false;
}

void nvnc_composite_fb_validate(const struct nvnc_composite_fb* self)
{
	nvnc_assert(!nvnc_composite_fb_contains_overlaps(self),
			"Composites may not contain overlapping framebuffers");
	nvnc_assert(nvnc_composite_fb_starts_at_zero(self),
			"Composites must start at (x, y) = (0, 0)");
}
