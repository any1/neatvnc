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

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "neatvnc.h"

struct gbm_bo;

struct nvnc_buffer {
	int ref;
	enum nvnc_fb_type type;
	bool is_external;

	/* main memory buffer */
	void* addr;

	/* dmabuf attributes */
	struct gbm_bo* bo;
	void* bo_map_handle;
};

struct nvnc_buffer* nvnc_buffer_new(size_t size);
struct nvnc_buffer* nvnc_buffer_from_addr(void* addr);
struct nvnc_buffer* nvnc_buffer_from_gbm_bo(struct gbm_bo* bo);

void nvnc_buffer_ref(struct nvnc_buffer* buffer);
void nvnc_buffer_unref(struct nvnc_buffer* buffer);

int nvnc_buffer_map(struct nvnc_buffer* buffer, uint16_t width, uint16_t height,
		int32_t* stride_out);
void nvnc_buffer_unmap(struct nvnc_buffer* buffer);
