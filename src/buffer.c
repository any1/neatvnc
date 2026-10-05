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

#include "buffer.h"
#include "buffer-pool.h"
#include "pixels.h"
#include "logging.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "config.h"

#ifdef HAVE_GBM
#include <gbm.h>
#endif

#define EXPORT __attribute__((visibility("default")))
#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_UP(n, a) (UDIV_UP(n, a) * a)

EXPORT
struct nvnc_buffer* nvnc_buffer_new(size_t size)
{
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_BUFFER_SIMPLE;

	size_t alignment = MAX(4, sizeof(void*));
	size_t aligned_size = ALIGN_UP(size, alignment);

	buffer->addr = aligned_alloc(alignment, aligned_size);
	if (!buffer->addr) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

EXPORT
struct nvnc_buffer* nvnc_buffer_from_addr(void* addr)
{
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_BUFFER_SIMPLE;
	buffer->is_external = true;
	buffer->addr = addr;

	return buffer;
}

EXPORT
struct nvnc_buffer* nvnc_buffer_from_gbm_bo(struct gbm_bo* bo)
{
#ifdef HAVE_GBM
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_BUFFER_GBM_BO;
	buffer->is_external = true;
	buffer->bo = bo;
	pthread_mutex_init(&buffer->map_mutex, NULL);

	return buffer;
#else
	nvnc_log(NVNC_LOG_ERROR, "nvnc_buffer_from_gbm_bo was not enabled during build time");
	return NULL;
#endif
}

EXPORT
struct nvnc_buffer* nvnc_buffer_from_custom(const struct nvnc_buffer_impl* impl,
		size_t size)
{
	if (size > sizeof(struct nvnc_buffer_impl)) {
		nvnc_log(NVNC_LOG_ERROR, "nvnc_buffer_impl is newer than implementation");
		return NULL;
	}

	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_BUFFER_CUSTOM;
	buffer->is_external = true;

	memcpy(&buffer->impl, impl, size);

	if (!buffer->impl.map || !buffer->impl.unmap) {
		nvnc_log(NVNC_LOG_ERROR, "nvnc_buffer_impl is missing required methods");
		free(buffer);
		return NULL;
	}

	return buffer;
}

EXPORT
void nvnc_buffer_ref(struct nvnc_buffer* buffer)
{
	buffer->ref++;
}

static void nvnc__buffer_free_internal(struct nvnc_buffer* buffer)
{
	switch (buffer->type) {
	case NVNC_BUFFER_UNSPEC:
		abort();
	case NVNC_BUFFER_SIMPLE:
		free(buffer->addr);
		break;
	case NVNC_BUFFER_GBM_BO:
#ifdef HAVE_GBM
		gbm_bo_destroy(buffer->bo);
#else
		abort();
#endif
		break;
	case NVNC_BUFFER_CUSTOM:
		abort();
	}
}

EXPORT
void nvnc_buffer_unref(struct nvnc_buffer* buffer)
{
	if (!buffer || --buffer->ref != 0)
		return;

	nvnc_buffer_unmap(buffer);

	struct nvnc_buffer_pool* pool =
		WEAKREF_CAST(buffer->pool, struct nvnc_buffer_pool, weakref);
	if (pool) {
		weakref_observer_deinit(&buffer->pool);
		buffer->ref = 1;
		TAILQ_INSERT_TAIL(&pool->buffers, buffer, link);
		return;
	}

	if (!buffer->is_external)
		nvnc__buffer_free_internal(buffer);

	if (buffer->cleanup_fn)
		buffer->cleanup_fn(buffer->userdata);

	// The cleanup function can save the buffer from deletion and put it
	// into its own buffer pool.
	if (buffer->ref == 0) {
		if (buffer->type == NVNC_BUFFER_GBM_BO)
			pthread_mutex_destroy(&buffer->map_mutex);
		free(buffer);
	}
}

int nvnc_buffer_map(struct nvnc_buffer* buffer, uint16_t width, uint16_t height,
		int32_t* stride_out)
{
	if (buffer->type == NVNC_BUFFER_CUSTOM) {
		if (buffer->addr)
			return 0;
		return buffer->impl.map(buffer, &buffer->addr, stride_out);
	}

#ifdef HAVE_GBM
	if (buffer->type != NVNC_BUFFER_GBM_BO)
		return 0;

	int rc = -1;
	uint32_t stride = 0;

	pthread_mutex_lock(&buffer->map_mutex);

	if (buffer->bo_map_handle)
		goto success;

	buffer->addr = gbm_bo_map(buffer->bo, 0, 0, width, height,
			GBM_BO_TRANSFER_READ, &stride, &buffer->bo_map_handle);
	if (!buffer->addr) {
		buffer->bo_map_handle = NULL;
		goto failure;
	}

	*stride_out = stride;
success:
	rc = 0;
failure:
	pthread_mutex_unlock(&buffer->map_mutex);
	return rc;
#else
	return 0;
#endif
}

void nvnc_buffer_unmap(struct nvnc_buffer* buffer)
{
	if (buffer->type == NVNC_BUFFER_CUSTOM) {
		if (buffer->addr)
			buffer->impl.unmap(buffer);
		buffer->addr = NULL;
		return;
	}

#ifdef HAVE_GBM
	if (buffer->type != NVNC_BUFFER_GBM_BO)
		return;

	pthread_mutex_lock(&buffer->map_mutex);

	if (buffer->bo_map_handle)
		gbm_bo_unmap(buffer->bo, buffer->bo_map_handle);

	buffer->bo_map_handle = NULL;
	buffer->addr = NULL;

	pthread_mutex_unlock(&buffer->map_mutex);
#endif
}
