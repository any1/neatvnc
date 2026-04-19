/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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

#include "frame.h"
#include "buffer.h"
#include "pixels.h"
#include "neatvnc.h"
#include "transform-util.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdatomic.h>

#include "config.h"

#ifdef HAVE_GBM
#include <gbm.h>
#endif

#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_frame* nvnc_frame_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride)
{
	struct nvnc_frame* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	uint32_t bpp = nvnc__pixel_size_from_fourcc(fourcc_format);
	size_t size = height * stride * bpp;

	fb->buffer = nvnc_buffer_new(size);
	if (!fb->buffer) {
		free(fb);
		return NULL;
	}

	fb->ref = 1;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->stride = stride;
	fb->pts = NVNC_NO_PTS;
	pixman_region_init_rect(&fb->damage, 0, 0, width, height);

	return fb;
}

EXPORT
struct nvnc_frame* nvnc_frame_from_buffer(struct nvnc_buffer* buffer, uint16_t width,
		uint16_t height, uint32_t fourcc_format, int32_t stride)
{
	struct nvnc_frame* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->buffer = buffer;
	nvnc_buffer_ref(buffer);

	fb->ref = 1;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->stride = stride;
	fb->pts = NVNC_NO_PTS;
	pixman_region_init_rect(&fb->damage, 0, 0, width, height);

	return fb;
}

EXPORT
struct nvnc_frame* nvnc_frame_from_raw(void* buffer, uint16_t width, uint16_t height,
		uint32_t fourcc_format, int32_t stride)
{
	struct nvnc_frame* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->buffer = nvnc_buffer_from_addr(buffer);
	if (!fb->buffer) {
		free(fb);
		return NULL;
	}

	fb->ref = 1;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->stride = stride;
	fb->pts = NVNC_NO_PTS;
	pixman_region_init_rect(&fb->damage, 0, 0, width, height);

	return fb;
}

EXPORT
struct nvnc_frame* nvnc_frame_from_gbm_bo(struct gbm_bo* bo)
{
#ifdef HAVE_GBM
	struct nvnc_frame* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->buffer = nvnc_buffer_from_gbm_bo(bo);
	if (!fb->buffer) {
		free(fb);
		return NULL;
	}

	fb->ref = 1;
	fb->width = gbm_bo_get_width(bo);
	fb->height = gbm_bo_get_height(bo);
	fb->fourcc_format = gbm_bo_get_format(bo);
	fb->stride = 0;
	fb->pts = NVNC_NO_PTS;
	pixman_region_init_rect(&fb->damage, 0, 0, fb->width, fb->height);

	return fb;
#else
	nvnc_log(NVNC_LOG_ERROR, "nvnc_frame_from_gbm_bo was not enabled during build time");
	return NULL;
#endif
}

EXPORT
struct nvnc_buffer* nvnc_frame_get_buffer(const struct nvnc_frame* fb)
{
	return fb->buffer;
}

EXPORT
void* nvnc_frame_get_addr(const struct nvnc_frame* fb)
{
	return fb->buffer->addr;
}

EXPORT
uint16_t nvnc_frame_get_width(const struct nvnc_frame* fb)
{
	return fb->width;
}

EXPORT
uint16_t nvnc_frame_get_height(const struct nvnc_frame* fb)
{
	return fb->height;
}

EXPORT
uint16_t nvnc_frame_get_logical_width(const struct nvnc_frame* fb)
{
	return fb->logical_width;
}

EXPORT
uint16_t nvnc_frame_get_logical_height(const struct nvnc_frame* fb)
{
	return fb->logical_height;
}

EXPORT
uint32_t nvnc_frame_get_fourcc_format(const struct nvnc_frame* fb)
{
	return fb->fourcc_format;
}

EXPORT
int32_t nvnc_frame_get_stride(const struct nvnc_frame* fb)
{
	return fb->stride;
}

EXPORT
int nvnc_frame_get_pixel_size(const struct nvnc_frame* fb)
{
	return nvnc__pixel_size_from_fourcc(fb->fourcc_format);
}

EXPORT
struct gbm_bo* nvnc_frame_get_gbm_bo(const struct nvnc_frame* fb)
{
	return fb->buffer->bo;
}

EXPORT
enum nvnc_transform nvnc_frame_get_transform(const struct nvnc_frame* fb)
{
	return fb->transform;
}

EXPORT
enum nvnc_buffer_type nvnc_frame_get_type(const struct nvnc_frame* fb)
{
	return fb->buffer->type;
}

EXPORT
uint64_t nvnc_frame_get_pts(const struct nvnc_frame* fb)
{
	return fb->pts;
}

EXPORT
void nvnc_frame_get_damage(const struct nvnc_frame* self,
		struct pixman_region16* damage)
{
	pixman_region_copy(damage, &self->damage);
}

static void nvnc__fb_free(struct nvnc_frame* fb)
{
	nvnc_cleanup_fn cleanup = fb->common.cleanup_fn;
	if (cleanup)
		cleanup(fb->common.userdata);

	nvnc_buffer_unref(fb->buffer);
	pixman_region_fini(&fb->damage);
	free(fb);
}

EXPORT
void nvnc_frame_ref(struct nvnc_frame* fb)
{
	fb->ref++;
}

EXPORT
void nvnc_frame_unref(struct nvnc_frame* fb)
{
	if (fb && --fb->ref == 0)
		nvnc__fb_free(fb);
}

EXPORT
void nvnc_frame_set_transform(struct nvnc_frame* fb, enum nvnc_transform transform)
{
	fb->transform = transform;
}

EXPORT
void nvnc_frame_set_logical_width(struct nvnc_frame* fb, uint16_t value)
{
	fb->logical_width = value;
}

EXPORT
void nvnc_frame_set_logical_height(struct nvnc_frame* fb, uint16_t value)
{
	fb->logical_height = value;
}

EXPORT
void nvnc_frame_set_pts(struct nvnc_frame* fb, uint64_t pts)
{
	fb->pts = pts;
}

EXPORT
void nvnc_frame_set_damage(struct nvnc_frame* self,
		const struct pixman_region16* damage)
{
	pixman_region_copy(&self->damage, damage);
}

int nvnc_frame_map(struct nvnc_frame* fb)
{
	int32_t byte_stride = 0;
	int rc = nvnc_buffer_map(fb->buffer, fb->width, fb->height, &byte_stride);
	/* byte_stride is only set for GBM BO buffers; skip for others */
	if (rc == 0 && byte_stride != 0)
		fb->stride = byte_stride / nvnc_frame_get_pixel_size(fb);
	return rc;
}

void nvnc_frame_unmap(struct nvnc_frame* fb)
{
	nvnc_buffer_unmap(fb->buffer);
}

void nvnc_composite_fb_init(struct nvnc_composite_fb* self,
		struct nvnc_frame* fbs[])
{
	int i;
	for (i = 0; fbs[i]; ++i) {
		self->fbs[i] = fbs[i];
	}
	self->n_fbs = i;
}

struct nvnc_frame_metadata* nvnc_frame_metadata_new(void)
{
	struct nvnc_frame_metadata* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	self->ref = 1;
	return self;
}

void nvnc_frame_metadata_ref(struct nvnc_frame_metadata* self)
{
	self->ref++;
}

void nvnc_frame_metadata_unref(struct nvnc_frame_metadata* self)
{
	if (self && --self->ref == 0)
		free(self);
}

void nvnc_composite_fb_ref(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_frame* fb = self->fbs[i];
		assert(fb);
		nvnc_frame_ref(fb);
	}
	if (self->metadata)
		nvnc_frame_metadata_ref(self->metadata);
}

void nvnc_composite_fb_unref(struct nvnc_composite_fb* self)
{
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_frame* fb = self->fbs[i];
		assert(fb);
		nvnc_frame_unref(fb);
	}
	nvnc_frame_metadata_unref(self->metadata);
}

int nvnc_composite_fb_map(struct nvnc_composite_fb* self)
{
	int rc = 0;
	for (int i = 0; i < self->n_fbs; ++i) {
		struct nvnc_frame* fb = self->fbs[i];
		assert(fb);
		if (nvnc_frame_map(fb) < 0)
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
		const struct nvnc_frame* fb = self->fbs[i];
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
	assert(self->n_fbs > 0);
	int64_t base = self->fbs[0]->pts;
	int64_t latest = base;

	// The point of rebasing the pts is to deal with overflow, although it
	// is astronomically unlikely to happen. :p
	for (int i = 1; i < self->n_fbs; ++i) {
		int64_t pts = self->fbs[i]->pts;
		int64_t latest_diff = latest - base;
		int64_t diff = pts - base;

		if (diff > latest_diff)
			latest = pts;
	}

	return latest;
}

static bool nvnc_frames_overlap(const struct nvnc_frame* a, const struct nvnc_frame* b)
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
		struct nvnc_frame* fb = self->fbs[i];
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
		struct nvnc_frame* a = self->fbs[i];
		assert(a);

		for (int j = i + 1; j < self->n_fbs; ++j) {
			struct nvnc_frame* b = self->fbs[j];
			assert(b);

			return nvnc_frames_overlap(a, b);
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
