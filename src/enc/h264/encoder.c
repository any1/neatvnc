/*
 * Copyright (c) 2024 Andri Yngvason
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

#include "enc/h264-encoder.h"
#include "config.h"

#ifdef HAVE_FFMPEG
extern struct h264_encoder_impl h264_encoder_ffmpeg_impl;
#endif

#ifdef HAVE_V4L2
extern struct h264_encoder_impl h264_encoder_v4l2m2m_impl;
#endif

struct h264_encoder* h264_encoder_create(uint32_t width, uint32_t height,
		uint32_t format, int quality)
{
	struct h264_encoder* encoder = NULL;

#ifdef HAVE_V4L2
	encoder = h264_encoder_v4l2m2m_impl.create(width, height, format, quality);
	if (encoder) {
		return encoder;
	}
#endif

#ifdef HAVE_FFMPEG
	encoder = h264_encoder_ffmpeg_impl.create(width, height, format, quality);
	if (encoder) {
		return encoder;
	}
#endif

	return encoder;
}

void h264_encoder_destroy(struct h264_encoder* self)
{
	self->impl->destroy(self);
}

void h264_encoder_set_packet_handler_fn(struct h264_encoder* self,
		h264_encoder_packet_handler_fn fn)
{
	self->on_packet_ready = fn;
}

void h264_encoder_set_userdata(struct h264_encoder* self, void* userdata)
{
	self->userdata = userdata;
}

void h264_encoder_feed(struct h264_encoder* self, struct nvnc_fb* fb)
{
	self->impl->feed(self, fb);
}

void h264_encoder_request_keyframe(struct h264_encoder* self)
{
	self->next_frame_should_be_keyframe = true;
}
