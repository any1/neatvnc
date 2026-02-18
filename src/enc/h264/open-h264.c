/*
 * Copyright (c) 2021 - 2025 Andri Yngvason
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
#include "pixman.h"
#include "rfb-proto.h"
#include "enc/util.h"
#include "vec.h"
#include "fb.h"
#include "enc/encoder.h"
#include "usdt.h"
#include "neatvnc.h"

#include <fcntl.h>
#include <stdlib.h>
#include <math.h>

#define OPEN_H264_MAX_CONTEXTS 64

typedef void (*open_h264_ready_fn)(void*);

struct open_h264_header {
	uint32_t length;
	uint32_t flags;
} RFB_PACKED;

struct open_h264_context {
	struct open_h264 *parent;

	struct h264_encoder* encoder;

	struct vec pending;

	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;

	uint32_t format;

	bool needs_reset;
	bool quality_changed;

	uint64_t last_pts;
};

struct open_h264 {
	struct encoder parent;

	struct open_h264_context* context[OPEN_H264_MAX_CONTEXTS];
	int n_contexts;

	int frame_barrier;
	uint16_t frame_width;
	uint16_t frame_height;

	int quality;

	bool needs_full_reset;
};

enum open_h264_flags {
	OPEN_H264_FLAG_RESET_CONTEXT = 0,
	OPEN_H264_FLAG_RESET_ALL_CONTEXTS = 1,
};

// TODO: Add some method to remove contexts when displays are removed

struct encoder* open_h264_new(void);

struct encoder_impl encoder_impl_open_h264;

static inline struct open_h264* open_h264(struct encoder* enc)
{
	return (struct open_h264*)enc;
}

static void open_h264_context_destroy(struct open_h264_context* ctx)
{
	if (ctx->encoder)
		h264_encoder_destroy(ctx->encoder);
	vec_destroy(&ctx->pending);
	free(ctx);
}

static void open_h264_destroy_all_contexts(struct open_h264* self)
{
	for (int i = 0; i < self->n_contexts; ++i) {
		struct open_h264_context* ctx = self->context[i];
		assert(ctx);

		open_h264_context_destroy(ctx);
	}

	self->n_contexts = 0;
}

static void open_h264_finish_frame(struct open_h264* self)
{
	int n_rects = 0;
	uint64_t pts = 0;
	struct vec buffer;
	vec_init(&buffer, 4096); // TODO: Calculate size

	for (int i = 0; i < self->n_contexts; ++i) {
		struct open_h264_context* context = self->context[i];
		assert(context);

		if (context->pending.len == 0)
			continue;

		uint32_t flags = context->needs_reset ?
			OPEN_H264_FLAG_RESET_CONTEXT : 0;
		context->needs_reset = false;

		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_OPEN_H264),
			.width = htons(context->width),
			.height = htons(context->height),
			.x = htons(context->x),
			.y = htons(context->y),
		};

		struct open_h264_header header = {
			.length = htonl(context->pending.len),
			.flags = htonl(flags),
		};

		nvnc_trace("Encoding rect at (x, y) = (%d, %d), size: %dx%d",
				context->x, context->y, context->width,
				context->height);

		vec_append(&buffer, &rect, sizeof(rect));
		vec_append(&buffer, &header, sizeof(header));
		vec_append(&buffer, context->pending.data,
				context->pending.len);
		vec_clear(&context->pending);

		pts = context->last_pts;

		n_rects++;
	}

	if (self->needs_full_reset) {
		self->needs_full_reset = false;
		open_h264_destroy_all_contexts(self);

		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_OPEN_H264),
		};

		uint32_t flags = OPEN_H264_FLAG_RESET_ALL_CONTEXTS;
		struct open_h264_header header = {
			.length = htonl(0),
			.flags = htonl(flags),
		};

		vec_append(&buffer, &rect, sizeof(rect));
		vec_append(&buffer, &header, sizeof(header));

		n_rects++;
	}

	struct encoded_frame* result;
	result = nvnc__encoded_frame_new(buffer.data, buffer.len,
			n_rects, self->frame_width, self->frame_height, pts);

	DTRACE_PROBE1(neatvnc, open_h264_finish_frame, pts);

	nvnc_trace("Finished encoding frame with %d rects, data length: %d",
			n_rects, buffer.len);

	encoder_finish_frame(&self->parent, result);

	encoded_frame_unref(result);
}

static void open_h264_handle_packet(const void* data, size_t size, uint64_t pts,
		void* userdata)
{
	struct open_h264_context* context = userdata;
	struct open_h264* self = context->parent;

	nvnc_trace("Got encoded packet for context %p", context);

	vec_append(&context->pending, data, size);
	context->last_pts = pts;

	assert(self->frame_barrier != 0);
	if (self->frame_barrier == 0)
		return;

	if (--self->frame_barrier != 0)
		return;

	open_h264_finish_frame(self);
}

struct encoder* open_h264_new(void)
{
	struct open_h264* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	encoder_init(&self->parent, &encoder_impl_open_h264);

	self->quality = 6;

	return (struct encoder*)self;
}

static void open_h264_destroy(struct encoder* enc)
{
	struct open_h264* self = open_h264(enc);
	open_h264_destroy_all_contexts(self);
	free(self);
}

static int open_h264_resize(struct open_h264_context* self, struct nvnc_fb* fb)
{
	int quality = 51 - round((50.0 / 9.0) * (float)self->parent->quality);

	struct h264_encoder* encoder = h264_encoder_create(fb->width,
			fb->height, fb->fourcc_format, quality);
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
	self->quality_changed = false;

	return 0;
}

static int open_h264_ctx_encode(struct open_h264_context* self, struct nvnc_fb* fb)
{
	DTRACE_PROBE1(neatvnc, open_h264_encode, fb->pts);

	if (fb->width != self->width || fb->height != self->height ||
			fb->fourcc_format != self->format ||
			self->quality_changed) {
		if (open_h264_resize(self, fb) < 0)
			return -1;
	}

	assert(self->width && self->height);

	// TODO: encoder_feed should return an error code
	h264_encoder_feed(self->encoder, fb);
	return 0;
}

static struct open_h264_context* open_h264_find_context(struct open_h264* self,
		uint16_t x, uint16_t y)
{
	for (int i = 0; i < self->n_contexts; ++i) {
		struct open_h264_context* ctx = self->context[i];
		assert(ctx);

		if (ctx->x == x && ctx->y == y)
			return ctx;
	}
	return NULL;
}

static struct open_h264_context* open_h264_context_new(struct open_h264* parent,
		uint16_t x, uint16_t y)
{
	if (parent->n_contexts >= OPEN_H264_MAX_CONTEXTS) {
		nvnc_log(NVNC_LOG_PANIC, "Maximum number of open-h264 contexts reached");
		return NULL;
	}

	struct open_h264_context* ctx = calloc(1, sizeof(*ctx));
	assert(ctx);

	ctx->parent = parent;
	ctx->x = x;
	ctx->y = y;

	vec_init(&ctx->pending, 4096);

	parent->context[parent->n_contexts++] = ctx;
	return ctx;
}

static struct open_h264_context* open_h264_get_context(struct open_h264* self,
		uint16_t x, uint16_t y)
{
	struct open_h264_context* ctx = open_h264_find_context(self, x, y);
	if (ctx)
		return ctx;

	return open_h264_context_new(self, x, y);
}

static bool region_intersects_box(struct pixman_region16* region,
		struct pixman_box16* box)
{
	pixman_region_overlap_t overlap =
		pixman_region_contains_rectangle(region, box);
	return overlap != PIXMAN_REGION_OUT;
}

static int open_h264_encode(struct encoder* enc,
		struct nvnc_composite_fb* composite,
		struct pixman_region16* damage)
{
	(void)damage;

	struct open_h264* self = open_h264(enc);

	assert(self->frame_barrier == 0);
	self->frame_barrier = 0;

	self->frame_width = nvnc_composite_fb_width(composite);
	self->frame_height = nvnc_composite_fb_height(composite);

	for (int i = 0; i < composite->n_fbs; ++i) {
		struct nvnc_fb* fb = composite->fbs[i];
		assert(fb);

		struct pixman_box16 box = {
			.x1 = fb->x_off,
			.y1 = fb->y_off,
			.x2 = fb->x_off + fb->width,
			.y2 = fb->y_off + fb->height,
		};

		if (!region_intersects_box(damage, &box))
			continue;

		struct open_h264_context* ctx =
			open_h264_get_context(self, fb->x_off, fb->y_off);

		int rc = open_h264_ctx_encode(ctx, fb);
		nvnc_assert(rc == 0, "Failed to encode frame");

		self->frame_barrier++;
	}

	nvnc_trace("Scheduled encoding for %d rects", self->frame_barrier);

	return 0;
}

static void open_h264_request_keyframe(struct encoder* enc)
{
	struct open_h264* self = open_h264(enc);
	for (int i = 0; i < self->n_contexts; ++i) {
		struct open_h264_context* ctx = self->context[i];
		assert(ctx);
		h264_encoder_request_keyframe(ctx->encoder);
	}
}

static void open_h264_set_quality(struct encoder* enc, int value)
{
	struct open_h264* self = open_h264(enc);
	if (value == 10)
		value = 6;
	for (int i = 0; i < self->n_contexts; ++i) {
		struct open_h264_context* ctx = self->context[i];
		assert(ctx);
		ctx->quality_changed |= self->quality != value;
	}
	self->quality = value;
}

static void open_h264_reset(struct encoder* enc)
{
	struct open_h264* self = open_h264(enc);
	self->needs_full_reset = true;
}

struct encoder_impl encoder_impl_open_h264 = {
	.flags = ENCODER_IMPL_FLAG_IGNORES_DAMAGE,
	.destroy = open_h264_destroy,
	.encode = open_h264_encode,
	.request_key_frame = open_h264_request_keyframe,
	.set_quality = open_h264_set_quality,
	.reset = open_h264_reset,
};
