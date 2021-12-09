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
#include "encoder.h"

#include <stdlib.h>

struct encoder* raw_encoder_new(void);
struct encoder* zrle_encoder_new(void);
struct encoder* tight_encoder_new(uint16_t width, uint16_t height);

extern struct encoder_impl encoder_impl_raw;
extern struct encoder_impl encoder_impl_zrle;
extern struct encoder_impl encoder_impl_tight;

struct encoder* encoder_new(enum rfb_encodings type, uint16_t width,
		uint16_t height)
{
	switch (type) {
	case RFB_ENCODING_RAW: return raw_encoder_new();
	case RFB_ENCODING_ZRLE: return zrle_encoder_new();
	case RFB_ENCODING_TIGHT: return tight_encoder_new(width, height);
	default: break;
	}

	return NULL;
}

enum rfb_encodings encoder_get_type(const struct encoder* self)
{
	if (self->impl == &encoder_impl_raw)
		return RFB_ENCODING_RAW;
	if (self->impl == &encoder_impl_zrle)
		return RFB_ENCODING_ZRLE;
	if (self->impl == &encoder_impl_tight)
		return RFB_ENCODING_TIGHT;

	abort();
	return 0;
}

void encoder_destroy(struct encoder* self)
{
	if (!self)
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

void encoder_set_tight_quality(struct encoder* self, int value)
{
	if (self->impl->set_tight_quality)
		self->impl->set_tight_quality(self, value);
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
	if (self->impl->encode)
		return self->impl->encode(self, fb, damage);

	abort();
	return -1;
}
