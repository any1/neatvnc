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

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "neatvnc.h"
#include "weakref.h"
#include "sys/queue.h"
#include "common.h"

struct gbm_bo;

struct nvnc_buffer {
	struct nvnc_common common;

	int ref;
	enum nvnc_frame_type type;
	bool is_external;

	/* main memory buffer */
	void* addr;

	/* dmabuf attributes */
	struct gbm_bo* bo;
	void* bo_map_handle;

	struct weakref_observer pool;
	TAILQ_ENTRY(nvnc_buffer) link;
};

int nvnc_buffer_map(struct nvnc_buffer* buffer, uint16_t width, uint16_t height,
		int32_t* stride_out);
void nvnc_buffer_unmap(struct nvnc_buffer* buffer);
