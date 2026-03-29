/*
 * Copyright (c) 2026 Andri Yngvason
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

#include "buffer-pool.h"
#include "buffer.h"
#include "weakref.h"

#include <assert.h>
#include <stdlib.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_buffer_pool* nvnc_buffer_pool_new(nvnc_buffer_alloc_fn alloc_fn)
{
	struct nvnc_buffer_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;
	weakref_subject_init(&self->weakref);
	TAILQ_INIT(&self->buffers);
	self->alloc_fn = alloc_fn;

	return self;
}

static void nvnc_buffer_pool__destroy(struct nvnc_buffer_pool* self)
{
	// Notify in-flight buffers so they free instead of returning to us
	weakref_subject_deinit(&self->weakref);

	while (!TAILQ_EMPTY(&self->buffers)) {
		struct nvnc_buffer* buffer = TAILQ_FIRST(&self->buffers);
		TAILQ_REMOVE(&self->buffers, buffer, link);
		nvnc_buffer_unref(buffer);
	}

	free(self);
}

EXPORT
void nvnc_buffer_pool_ref(struct nvnc_buffer_pool* self)
{
	self->ref++;
}

EXPORT
void nvnc_buffer_pool_unref(struct nvnc_buffer_pool* self)
{
	if (!self || --self->ref != 0)
		return;
	nvnc_buffer_pool__destroy(self);
}

static struct nvnc_buffer* nvnc_buffer_pool__acquire_new(
		struct nvnc_buffer_pool* self)
{
	struct nvnc_buffer* buffer = self->alloc_fn(self);
	if (!buffer)
		return NULL;

	weakref_observer_init(&buffer->pool, &self->weakref);
	return buffer;
}

static struct nvnc_buffer* nvnc_buffer_pool__acquire_from_queue(
		struct nvnc_buffer_pool* self)
{
	struct nvnc_buffer* buffer = TAILQ_FIRST(&self->buffers);
	assert(buffer);
	TAILQ_REMOVE(&self->buffers, buffer, link);
	weakref_observer_init(&buffer->pool, &self->weakref);
	return buffer;
}

EXPORT
struct nvnc_buffer* nvnc_buffer_pool_acquire(struct nvnc_buffer_pool* self)
{
	return TAILQ_EMPTY(&self->buffers) ?
		nvnc_buffer_pool__acquire_new(self) :
		nvnc_buffer_pool__acquire_from_queue(self);
}
