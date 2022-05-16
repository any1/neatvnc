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

#include <stdint.h>

struct nvnc_fb;
struct pixman_region16;

struct resampler;

typedef void (*resampler_fn)(struct nvnc_fb*, struct pixman_region16* damage,
			     void* userdata);

struct resampler* resampler_create(void);
void resampler_destroy(struct resampler*);

int resampler_feed(struct resampler*, struct nvnc_fb* fb,
		struct pixman_region16* damage, resampler_fn on_done,
		void* userdata);

void resample_now(struct nvnc_fb* dst, struct nvnc_fb* src,
		struct pixman_region16* damage);
