/*
 * Copyright (c) 2021 - 2022 Andri Yngvason
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

#include "rfb-proto.h"

#include <stdint.h>
#include <unistd.h>

struct encoder;
struct nvnc_fb;
struct pixman_region16;
struct rcbuf;

enum encoder_impl_flags {
	ENCODER_IMPL_FLAG_NONE = 0,
	ENCODER_IMPL_FLAG_IGNORES_DAMAGE = 1 << 0,
};

struct encoder_impl {
	enum encoder_impl_flags flags;

	void (*destroy)(struct encoder*);

	void (*set_output_format)(struct encoder*,
			const struct rfb_pixel_format*);
	void (*set_quality)(struct encoder*, int quality);

	int (*resize)(struct encoder*, uint16_t width, uint16_t height);

	int (*encode)(struct encoder*, struct nvnc_fb* fb,
			struct pixman_region16* damage);

	void (*request_key_frame)(struct encoder*);
};

struct encoder {
	struct encoder_impl* impl;

	int ref;

	uint16_t x_pos;
	uint16_t y_pos;

	int n_rects;

	void (*on_done)(struct encoder*, struct rcbuf* result, uint64_t pts);
	void* userdata;
};

struct encoder* encoder_new(enum rfb_encodings type, uint16_t width,
		uint16_t height);
void encoder_ref(struct encoder* self);
void encoder_unref(struct encoder* self);

void encoder_init(struct encoder* self, struct encoder_impl*);

enum rfb_encodings encoder_get_type(const struct encoder* self);
enum encoder_kind encoder_get_kind(const struct encoder* self);

void encoder_set_output_format(struct encoder* self,
		const struct rfb_pixel_format*);
void encoder_set_quality(struct encoder* self, int value);

int encoder_resize(struct encoder* self, uint16_t width, uint16_t height);

int encoder_encode(struct encoder* self, struct nvnc_fb* fb,
		struct pixman_region16* damage);

void encoder_request_key_frame(struct encoder* self);

void encoder_finish_frame(struct encoder* self, struct rcbuf* result,
		uint64_t pts);
