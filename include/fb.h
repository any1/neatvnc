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

#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "neatvnc.h"
#include "common.h"

#define NVNC_FB_COMPOSITE_MAX 64

struct gbm_bo;

struct nvnc_fb {
	struct nvnc_common common;
	enum nvnc_fb_type type;
	int ref;
	int hold_count;
	nvnc_fb_release_fn on_release;
	void* release_context;
	bool is_external;
	uint16_t x_off;
	uint16_t y_off;
	uint16_t width;
	uint16_t height;
	uint16_t logical_width;
	uint16_t logical_height;
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

struct nvnc_composite_fb {
	struct nvnc_fb* fbs[NVNC_FB_COMPOSITE_MAX];
	int n_fbs;
};

void nvnc_fb_hold(struct nvnc_fb* fb);
void nvnc_fb_release(struct nvnc_fb* fb);
int nvnc_fb_map(struct nvnc_fb* fb);
void nvnc_fb_unmap(struct nvnc_fb* fb);

void nvnc_composite_fb_init(struct nvnc_composite_fb* self,
		struct nvnc_fb* fbs[]);
void nvnc_composite_fb_ref(struct nvnc_composite_fb* self);
void nvnc_composite_fb_unref(struct nvnc_composite_fb* self);
void nvnc_composite_fb_hold(struct nvnc_composite_fb* self);
void nvnc_composite_fb_release(struct nvnc_composite_fb* self);
int nvnc_composite_fb_map(struct nvnc_composite_fb* self);
void nvnc_composite_fb_copy(struct nvnc_composite_fb* dst,
		const struct nvnc_composite_fb* src);

uint16_t nvnc_composite_fb_width(const struct nvnc_composite_fb* self);
uint16_t nvnc_composite_fb_height(const struct nvnc_composite_fb* self);
uint64_t nvnc_composite_fb_pts(const struct nvnc_composite_fb* self);

void nvnc_composite_fb_validate(const struct nvnc_composite_fb* self);
