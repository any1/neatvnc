/*
 * Copyright (c) 2021 - 2026 Andri Yngvason
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

#include <stdlib.h>

#define EXPORT __attribute__((visibility("default")))

struct nvnc_frame_pool {
	int ref;

	struct nvnc_buffer_pool* buffer_pool;

	uint16_t width;
	uint16_t height;
	int32_t stride;
	uint32_t fourcc_format;
};

static struct nvnc_buffer* fb_pool_buffer_alloc(
		struct nvnc_buffer_pool* pool)
{
	struct nvnc_frame_pool* self = nvnc_get_userdata(pool);
	uint32_t bpp = nvnc__pixel_size_from_fourcc(self->fourcc_format);
	size_t size = (size_t)self->height * self->stride * bpp;
	return nvnc_buffer_new(size);
}

EXPORT
struct nvnc_frame_pool* nvnc_frame_pool_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride)
{
	struct nvnc_frame_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;
	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;

	self->buffer_pool = nvnc_buffer_pool_new(fb_pool_buffer_alloc);
	if (!self->buffer_pool) {
		free(self);
		return NULL;
	}
	nvnc_set_userdata(self->buffer_pool, self, NULL);

	return self;
}

static void nvnc_frame_pool__destroy(struct nvnc_frame_pool* self)
{
	nvnc_buffer_pool_unref(self->buffer_pool);
	free(self);
}

EXPORT
bool nvnc_frame_pool_resize(struct nvnc_frame_pool* self, uint16_t width,
		uint16_t height, uint32_t fourcc_format, uint16_t stride)
{
	if (width == self->width && height == self->height &&
			fourcc_format == self->fourcc_format &&
			stride == self->stride)
		return false;

	nvnc_buffer_pool_unref(self->buffer_pool);

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;

	self->buffer_pool = nvnc_buffer_pool_new(fb_pool_buffer_alloc);
	nvnc_set_userdata(self->buffer_pool, self, NULL);

	return true;
}

EXPORT
void nvnc_frame_pool_ref(struct nvnc_frame_pool* self)
{
	self->ref++;
}

EXPORT
void nvnc_frame_pool_unref(struct nvnc_frame_pool* self)
{
	if (--self->ref == 0)
		nvnc_frame_pool__destroy(self);
}

EXPORT
struct nvnc_frame* nvnc_frame_pool_acquire(struct nvnc_frame_pool* self)
{
	struct nvnc_buffer* buffer =
		nvnc_buffer_pool_acquire(self->buffer_pool);
	if (!buffer)
		return NULL;

	struct nvnc_frame* fb = calloc(1, sizeof(*fb));
	if (!fb) {
		nvnc_buffer_unref(buffer);
		return NULL;
	}

	fb->ref = 1;
	fb->width = self->width;
	fb->height = self->height;
	fb->fourcc_format = self->fourcc_format;
	fb->stride = self->stride;
	fb->pts = NVNC_NO_PTS;
	fb->buffer = buffer;

	return fb;
}
