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
#pragma once

#include "rfb-proto.h"

#include <stdint.h>
#include <unistd.h>

struct encoder;
struct nvnc_fb;
struct pixman_region16;
struct rcbuf;

struct encoder_impl {
	int (*init)(struct encoder*);
	void (*destroy)(struct encoder*);

	void (*set_output_format)(struct encoder*,
			const struct rfb_pixel_format*);
	void (*set_tight_quality)(struct encoder*, int quality);

	int (*resize)(struct encoder*, uint16_t width, uint16_t height);

	int (*encode)(struct encoder*, struct nvnc_fb* fb,
			struct pixman_region16* damage);
};

struct encoder {
	struct encoder_impl* impl;

	uint16_t x_pos;
	uint16_t y_pos;

	int n_rects;

	void (*on_done)(struct encoder*, struct rcbuf* result);
	void* userdata;
};

struct encoder* encoder_new(enum rfb_encodings type, uint16_t width,
		uint16_t height);
void encoder_destroy(struct encoder* self);

enum rfb_encodings encoder_get_type(const struct encoder* self);

void encoder_set_output_format(struct encoder* self,
		const struct rfb_pixel_format*);
void encoder_set_tight_quality(struct encoder* self, int value);

int encoder_resize(struct encoder* self, uint16_t width, uint16_t height);

int encoder_encode(struct encoder* self, struct nvnc_fb* fb,
		struct pixman_region16* damage);
