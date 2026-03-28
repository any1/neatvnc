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

#include "buffer.h"
#include "pixels.h"
#include "logging.h"

#include <stdlib.h>
#include <sys/param.h>

#include "config.h"

#ifdef HAVE_GBM
#include <gbm.h>
#endif

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_UP(n, a) (UDIV_UP(n, a) * a)

struct nvnc_buffer* nvnc_buffer_new(size_t size)
{
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_FB_SIMPLE;

	size_t alignment = MAX(4, sizeof(void*));
	size_t aligned_size = ALIGN_UP(size, alignment);

	buffer->addr = aligned_alloc(alignment, aligned_size);
	if (!buffer->addr) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

struct nvnc_buffer* nvnc_buffer_from_addr(void* addr)
{
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_FB_SIMPLE;
	buffer->is_external = true;
	buffer->addr = addr;

	return buffer;
}

struct nvnc_buffer* nvnc_buffer_from_gbm_bo(struct gbm_bo* bo)
{
#ifdef HAVE_GBM
	struct nvnc_buffer* buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->ref = 1;
	buffer->type = NVNC_FB_GBM_BO;
	buffer->is_external = true;
	buffer->bo = bo;

	return buffer;
#else
	nvnc_log(NVNC_LOG_ERROR, "nvnc_buffer_from_gbm_bo was not enabled during build time");
	return NULL;
#endif
}

void nvnc_buffer_ref(struct nvnc_buffer* buffer)
{
	buffer->ref++;
}

void nvnc_buffer_unref(struct nvnc_buffer* buffer)
{
	if (!buffer || --buffer->ref != 0)
		return;

	nvnc_buffer_unmap(buffer);

	if (!buffer->is_external)
		switch (buffer->type) {
		case NVNC_FB_UNSPEC:
			abort();
		case NVNC_FB_SIMPLE:
			free(buffer->addr);
			break;
		case NVNC_FB_GBM_BO:
#ifdef HAVE_GBM
			gbm_bo_destroy(buffer->bo);
#else
			abort();
#endif
			break;
		}

	free(buffer);
}

int nvnc_buffer_map(struct nvnc_buffer* buffer, uint16_t width, uint16_t height,
		int32_t* stride_out)
{
#ifdef HAVE_GBM
	if (buffer->type != NVNC_FB_GBM_BO || buffer->bo_map_handle)
		return 0;

	uint32_t stride = 0;
	buffer->addr = gbm_bo_map(buffer->bo, 0, 0, width, height,
			GBM_BO_TRANSFER_READ, &stride, &buffer->bo_map_handle);
	if (!buffer->addr) {
		buffer->bo_map_handle = NULL;
		return -1;
	}

	*stride_out = stride;
	return 0;
#else
	return 0;
#endif
}

void nvnc_buffer_unmap(struct nvnc_buffer* buffer)
{
#ifdef HAVE_GBM
	if (buffer->type != NVNC_FB_GBM_BO)
		return;

	if (buffer->bo_map_handle)
		gbm_bo_unmap(buffer->bo, buffer->bo_map_handle);

	buffer->bo_map_handle = NULL;
	buffer->addr = NULL;
#endif
}
