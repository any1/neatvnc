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
#include "enc/encoder.h"
#include "config.h"

#include <stdlib.h>
#include <assert.h>

struct encoder* raw_encoder_new(void);
struct encoder* zrle_encoder_new(void);
struct encoder* tight_encoder_new(uint16_t width, uint16_t height);
#ifdef ENABLE_OPEN_H264
struct encoder* open_h264_new(void);
#endif

extern struct encoder_impl encoder_impl_raw;
extern struct encoder_impl encoder_impl_zrle;
extern struct encoder_impl encoder_impl_tight;
#ifdef ENABLE_OPEN_H264
extern struct encoder_impl encoder_impl_open_h264;
#endif

struct encoder* encoder_new(enum rfb_encodings type, uint16_t width,
		uint16_t height)
{
	switch (type) {
	case RFB_ENCODING_RAW: return raw_encoder_new();
	case RFB_ENCODING_ZRLE: return zrle_encoder_new();
	case RFB_ENCODING_TIGHT: return tight_encoder_new(width, height);
#ifdef ENABLE_OPEN_H264
	case RFB_ENCODING_OPEN_H264: return open_h264_new();
#endif
	default: break;
	}

	return NULL;
}

void encoder_init(struct encoder* self, struct encoder_impl* impl)
{
	self->ref = 1;
	self->impl = impl;
}

enum rfb_encodings encoder_get_type(const struct encoder* self)
{
	if (self->impl == &encoder_impl_raw)
		return RFB_ENCODING_RAW;
	if (self->impl == &encoder_impl_zrle)
		return RFB_ENCODING_ZRLE;
	if (self->impl == &encoder_impl_tight)
		return RFB_ENCODING_TIGHT;
#ifdef ENABLE_OPEN_H264
	if (self->impl == &encoder_impl_open_h264)
		return RFB_ENCODING_OPEN_H264;
#endif

	abort();
	return 0;
}

void encoder_ref(struct encoder* self)
{
	assert(self->ref > 0);
	self->ref++;
}

void encoder_unref(struct encoder* self)
{
	if (!self)
		return;

	if (--self->ref != 0)
		return;

	if (self->impl->destroy)
		self->impl->destroy(self);
}

void encoder_set_output_format(struct encoder* self,
		const struct rfb_pixel_format* pixfmt)
{
	if (self->impl->set_output_format)
		self->impl->set_output_format(self, pixfmt);
}

void encoder_set_quality(struct encoder* self, int value)
{
	if (self->impl->set_quality)
		self->impl->set_quality(self, value);
}

int encoder_resize(struct encoder* self, uint16_t width, uint16_t height)
{
	if (self->impl->resize)
		return self->impl->resize(self, width, height);

	return 0;
}

int encoder_encode(struct encoder* self, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	assert(self->impl->encode);
	return self->impl->encode(self, fb, damage);
}

void encoder_request_key_frame(struct encoder* self)
{
	if (self->impl->request_key_frame)
		return self->impl->request_key_frame(self);
}

void encoder_finish_frame(struct encoder* self, struct rcbuf* result,
		uint64_t pts)
{
	if (self->on_done)
		self->on_done(self, result, pts);
}
