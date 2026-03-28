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

#include "fb.h"
#include "buffer.h"
#include "buffer-pool.h"
#include "pixels.h"
#include "neatvnc.h"

#include <stdlib.h>

#define EXPORT __attribute__((visibility("default")))

struct nvnc_fb_pool {
	int ref;

	struct nvnc_buffer_pool buffer_pool;

	uint16_t width;
	uint16_t height;
	int32_t stride;
	uint32_t fourcc_format;

	nvnc_fb_alloc_fn alloc_fn;
};

static struct nvnc_buffer* fb_pool__default_buffer_alloc(void* userdata)
{
	struct nvnc_fb_pool* pool = userdata;
	uint32_t bpp = nvnc__pixel_size_from_fourcc(pool->fourcc_format);
	size_t size = (size_t)pool->height * pool->stride * bpp;
	return nvnc_buffer_new(size);
}

static struct nvnc_buffer* fb_pool__custom_buffer_alloc(void* userdata)
{
	struct nvnc_fb_pool* pool = userdata;
	struct nvnc_fb* fb = pool->alloc_fn(pool->width, pool->height,
			pool->fourcc_format, pool->stride);
	if (!fb)
		return NULL;

	struct nvnc_buffer* buffer = fb->buffer;
	nvnc_buffer_ref(buffer);
	nvnc_fb_unref(fb);
	return buffer;
}

EXPORT
struct nvnc_fb_pool* nvnc_fb_pool_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride)
{
	struct nvnc_fb_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;
	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;

	nvnc_buffer_pool_init(&self->buffer_pool, fb_pool__default_buffer_alloc,
			self);

	return self;
}

static void nvnc_fb_pool__destroy(struct nvnc_fb_pool* self)
{
	nvnc_buffer_pool_deinit(&self->buffer_pool);
	free(self);
}

EXPORT
bool nvnc_fb_pool_resize(struct nvnc_fb_pool* self, uint16_t width,
		uint16_t height, uint32_t fourcc_format, uint16_t stride)
{
	if (width == self->width && height == self->height &&
			fourcc_format == self->fourcc_format &&
			stride == self->stride)
		return false;

	nvnc_buffer_pool_deinit(&self->buffer_pool);

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;

	nvnc_buffer_alloc_fn alloc_fn = self->alloc_fn ?
		fb_pool__custom_buffer_alloc : fb_pool__default_buffer_alloc;
	nvnc_buffer_pool_init(&self->buffer_pool, alloc_fn, self);

	return true;
}

EXPORT
void nvnc_fb_pool_ref(struct nvnc_fb_pool* self)
{
	self->ref++;
}

EXPORT
void nvnc_fb_pool_unref(struct nvnc_fb_pool* self)
{
	if (--self->ref == 0)
		nvnc_fb_pool__destroy(self);
}

static void nvnc_fb_pool__on_fb_release(struct nvnc_fb* fb, void* userdata)
{
	nvnc_buffer_unref(fb->buffer);
	fb->buffer = NULL;
}

EXPORT
struct nvnc_fb* nvnc_fb_pool_acquire(struct nvnc_fb_pool* self)
{
	struct nvnc_buffer* buffer =
		nvnc_buffer_pool_acquire(&self->buffer_pool);
	if (!buffer)
		return NULL;

	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
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

	nvnc_fb_set_release_fn(fb, nvnc_fb_pool__on_fb_release, NULL);

	return fb;
}

EXPORT
void nvnc_fb_pool_set_alloc_fn(struct nvnc_fb_pool* self, nvnc_fb_alloc_fn fn)
{
	self->alloc_fn = fn;

	nvnc_buffer_alloc_fn buffer_alloc_fn = fn ?
		fb_pool__custom_buffer_alloc : fb_pool__default_buffer_alloc;
	nvnc_buffer_pool_deinit(&self->buffer_pool);
	nvnc_buffer_pool_init(&self->buffer_pool, buffer_alloc_fn, self);
}
