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

#include "fb.h"
#include "neatvnc.h"

#include "sys/queue.h"

#include <stdlib.h>
#include <assert.h>

#define EXPORT __attribute__((visibility("default")))

struct fbq_item {
	struct nvnc_fb* fb;
	TAILQ_ENTRY(fbq_item) link;
};

TAILQ_HEAD(fbq, fbq_item);

struct nvnc_fb_pool {
	int ref;

	struct fbq fbs;

	uint16_t width;
	uint16_t height;
	int32_t stride;
	uint32_t fourcc_format;

	nvnc_fb_alloc_fn alloc_fn;
};

EXPORT
struct nvnc_fb_pool* nvnc_fb_pool_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride)
{
	struct nvnc_fb_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;

	TAILQ_INIT(&self->fbs);
	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;
	self->alloc_fn = nvnc_fb_new;

	return self;
}

static void nvnc_fb_pool__destroy_fbs(struct nvnc_fb_pool* self)
{
	while (!TAILQ_EMPTY(&self->fbs)) {
		struct fbq_item* item = TAILQ_FIRST(&self->fbs);
		TAILQ_REMOVE(&self->fbs, item, link);
		nvnc_fb_unref(item->fb);
		free(item);
	}
}

static void nvnc_fb_pool__destroy(struct nvnc_fb_pool* self)
{
	nvnc_fb_pool__destroy_fbs(self);
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

	nvnc_fb_pool__destroy_fbs(self);

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->fourcc_format = fourcc_format;

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
	struct nvnc_fb_pool* pool = userdata;

	nvnc_fb_pool_release(pool, fb);
	nvnc_fb_pool_unref(pool);
}

static struct nvnc_fb* nvnc_fb_pool__acquire_new(struct nvnc_fb_pool* self)
{
	struct nvnc_fb* fb = self->alloc_fn(self->width, self->height,
			self->fourcc_format, self->stride);
	if (!fb)
		return NULL;

	nvnc_fb_set_release_fn(fb, nvnc_fb_pool__on_fb_release, self);
	nvnc_fb_pool_ref(self);

	return fb;
}

static struct nvnc_fb* nvnc_fb_pool__acquire_from_list(struct nvnc_fb_pool* self)
{
	struct fbq_item* item = TAILQ_FIRST(&self->fbs);
	struct nvnc_fb* fb = item->fb;
	assert(item && fb);

	TAILQ_REMOVE(&self->fbs, item, link);
	free(item);

	nvnc_fb_pool_ref(self);

	return fb;
}

EXPORT
struct nvnc_fb* nvnc_fb_pool_acquire(struct nvnc_fb_pool* self)
{
	return TAILQ_EMPTY(&self->fbs) ?
		nvnc_fb_pool__acquire_new(self) :
		nvnc_fb_pool__acquire_from_list(self);
}

EXPORT
void nvnc_fb_pool_release(struct nvnc_fb_pool* self, struct nvnc_fb* fb)
{
	if (fb->width != self->width || fb->height != self->height ||
			fb->fourcc_format != self->fourcc_format ||
			fb->stride != self->stride) {
		return;
	}

	nvnc_fb_ref(fb);
	
	struct fbq_item* item = calloc(1, sizeof(*item));
	assert(item);
	item->fb = fb;
	TAILQ_INSERT_TAIL(&self->fbs, item, link);
}

EXPORT
void nvnc_fb_pool_set_alloc_fn(struct nvnc_fb_pool* self, nvnc_fb_alloc_fn fn)
{
	self->alloc_fn = fn;
}
