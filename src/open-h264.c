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

#include "h264-encoder.h"
#include "rfb-proto.h"
#include "enc-util.h"
#include "vec.h"
#include "fb.h"
#include "rcbuf.h"
#include "encoder.h"
#include "usdt.h"

#include <stdlib.h>

typedef void (*open_h264_ready_fn)(void*);

struct open_h264_header {
	uint32_t length;
	uint32_t flags;
} RFB_PACKED;

struct open_h264 {
	struct encoder parent;

	struct h264_encoder* encoder;

	struct vec pending;
	uint64_t pts;

	uint32_t width;
	uint32_t height;
	uint32_t format;

	bool needs_reset;
};

enum open_h264_flags {
	OPEN_H264_FLAG_RESET_CONTEXT = 0,
	OPEN_H264_FLAG_RESET_ALL_CONTEXTS = 1,
};

struct encoder* open_h264_new(void);
static struct rcbuf* open_h264_pull(struct encoder* enc, uint64_t* pts);

struct encoder_impl encoder_impl_open_h264;

static inline struct open_h264* open_h264(struct encoder* enc)
{
	return (struct open_h264*)enc;
}

static void open_h264_handle_packet(const void* data, size_t size, uint64_t pts,
		void* userdata)
{
	struct open_h264* self = userdata;

	// Let's not deplete the RAM if the client isn't pulling
	if (self->pending.len > 100000000) {
		// TODO: Drop buffer and request a keyframe?
		nvnc_log(NVNC_LOG_WARNING, "Pending buffer grew too large. Dropping packet...");
		return;
	}

	vec_append(&self->pending, data, size);
	self->pts = pts;

	uint64_t rpts = NVNC_NO_PTS;
	struct rcbuf* result = open_h264_pull(&self->parent, &rpts);

	DTRACE_PROBE1(neatvnc, open_h264_finish_frame, rpts);

	encoder_finish_frame(&self->parent, result, rpts);
}

static int open_h264_init_pending(struct open_h264* self)
{
	if (vec_init(&self->pending, 4096) < 0)
		return -1;

	vec_append_zero(&self->pending, sizeof(struct rfb_server_fb_rect));
	vec_append_zero(&self->pending, sizeof(struct open_h264_header));

	return 0;
}

struct encoder* open_h264_new(void)
{
	struct open_h264* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &encoder_impl_open_h264;

	if (open_h264_init_pending(self) < 0) {
		free(self);
		return NULL;
	}

	self->pts = NVNC_NO_PTS;

	return (struct encoder*)self;
}

static void open_h264_destroy(struct encoder* enc)
{
	struct open_h264* self = open_h264(enc);

	if (self->encoder)
		h264_encoder_destroy(self->encoder);
	vec_destroy(&self->pending);

	free(self);
}

static int open_h264_resize(struct open_h264* self, struct nvnc_fb* fb)
{
	struct h264_encoder* encoder = h264_encoder_create(fb->width,
			fb->height, fb->fourcc_format);
	if (!encoder)
		return -1;

	if (self->encoder)
		h264_encoder_destroy(self->encoder);

	h264_encoder_set_userdata(encoder, self);
	h264_encoder_set_packet_handler_fn(encoder, open_h264_handle_packet);

	self->encoder = encoder;

	self->width = fb->width;
	self->height = fb->height;
	self->format = fb->fourcc_format;
	self->needs_reset = true;

	return 0;
}

static int open_h264_encode(struct encoder* enc, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	DTRACE_PROBE1(neatvnc, open_h264_encode, fb->pts);

	struct open_h264* self = open_h264(enc);
	(void)damage;

	if (fb->width != self->width || fb->height != self->height ||
			fb->fourcc_format != self->format) {
		if (open_h264_resize(self, fb) < 0)
			return -1;
	}

	assert(self->width && self->height);

	// TODO: encoder_feed should return an error code
	h264_encoder_feed(self->encoder, fb);
	return 0;
}

static struct rcbuf* open_h264_pull(struct encoder* enc, uint64_t* pts)
{
	struct open_h264* self = open_h264(enc);

	size_t payload_size = self->pending.len
		- sizeof(struct rfb_server_fb_rect)
		- sizeof(struct open_h264_header);
	if (payload_size == 0)
		return NULL;

	if (pts)
		*pts = self->pts;
	self->pts = NVNC_NO_PTS;

	uint32_t flags = self->needs_reset ? OPEN_H264_FLAG_RESET_CONTEXT : 0;
	self->needs_reset = false;

	struct rfb_server_fb_rect* rect = self->pending.data;
	rect->encoding = htonl(RFB_ENCODING_OPEN_H264);
	rect->width = htons(self->width);
	rect->height = htons(self->height);
	rect->x = htons(self->parent.x_pos);
	rect->y = htons(self->parent.y_pos);

	struct open_h264_header* header =
		(void*)(((uint8_t*)self->pending.data) + sizeof(*rect));
	header->length = htonl(payload_size);
	header->flags = htonl(flags);

	enc->n_rects = 1;

	struct rcbuf* payload = rcbuf_new(self->pending.data, self->pending.len);

	open_h264_init_pending(self);
	return payload;
}

static void open_h264_request_keyframe(struct encoder* enc)
{
	struct open_h264* self = open_h264(enc);
	h264_encoder_request_keyframe(self->encoder);
}

struct encoder_impl encoder_impl_open_h264 = {
	.flags = ENCODER_IMPL_FLAG_IGNORES_DAMAGE,
	.destroy = open_h264_destroy,
	.encode = open_h264_encode,
	.request_key_frame = open_h264_request_keyframe,
};
