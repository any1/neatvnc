/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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

#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "neatvnc.h"
#include "common.h"

struct gbm_bo;

struct nvnc_fb {
	struct nvnc_common common;
	enum nvnc_fb_type type;
	int ref;
	int hold_count;
	nvnc_fb_release_fn on_release;
	void* release_context;
	bool is_external;
	uint16_t width;
	uint16_t height;
	uint32_t fourcc_format;
	enum nvnc_transform transform;
	uint64_t pts; // in micro seconds

	/* main memory buffer attributes */
	void* addr;
	int32_t stride;

	/* dmabuf attributes */
	struct gbm_bo* bo;
	void* bo_map_handle;
};

void nvnc_fb_hold(struct nvnc_fb* fb);
void nvnc_fb_release(struct nvnc_fb* fb);
int nvnc_fb_map(struct nvnc_fb* fb);
void nvnc_fb_unmap(struct nvnc_fb* fb);
