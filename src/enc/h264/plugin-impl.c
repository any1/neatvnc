/*
 * Copyright (c) 2026 Norbert Schultz
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
#include "neatvnc.h"

#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

struct nvnc_h264_encoder {
	struct h264_encoder base;
	const struct nvnc_h264_encoder_impl* impl;
	void* userdata;
};

struct h264_encoder_impl h264_encoder_plugin_impl;

static struct nvnc_h264_encoder_impl plugin_impl;

EXPORT
int nvnc_set_h264_encoder_impl(const struct nvnc_h264_encoder_impl* impl,
		size_t size)
{
	memset(&plugin_impl, 0, sizeof(plugin_impl));

	if (!impl)
		return 0;

	if (size > sizeof(plugin_impl)) {
		nvnc_log(NVNC_LOG_ERROR,
			"nvnc_h264_encoder_impl is newer than implementation");
		return -1;
	}

	memcpy(&plugin_impl, impl, size);

	if (!plugin_impl.create || !plugin_impl.destroy || !plugin_impl.feed) {
		nvnc_log(NVNC_LOG_ERROR,
			"nvnc_h264_encoder_impl is missing required methods");
		memset(&plugin_impl, 0, sizeof(plugin_impl));
		return -1;
	}

	return 0;
}

struct h264_encoder* h264_encoder_plugin_create(enum nvnc_buffer_type type,
		uint32_t width, uint32_t height, uint32_t format, int quality)
{
	if (!plugin_impl.create)
		return NULL;

	struct nvnc_h264_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.impl = &h264_encoder_plugin_impl;
	self->base.next_frame_should_be_keyframe = true;
	self->impl = &plugin_impl;

	if (plugin_impl.create(self, type, width, height, format, quality) < 0) {
		free(self);
		return NULL;
	}

	return &self->base;
}

static void h264_encoder_plugin_destroy(struct h264_encoder* base)
{
	struct nvnc_h264_encoder* self = (struct nvnc_h264_encoder*)base;
	self->impl->destroy(self);
	free(self);
}

static void h264_encoder_plugin_feed(struct h264_encoder* base,
		struct nvnc_frame* fb)
{
	struct nvnc_h264_encoder* self = (struct nvnc_h264_encoder*)base;
	enum nvnc_h264_encoder_frame_flags flags = 0;
	if (base->next_frame_should_be_keyframe)
		flags |= NVNC_H264_ENCODER_FRAME_KEYFRAME;
	base->next_frame_should_be_keyframe = false;
	self->impl->feed(self, fb, flags);
}

EXPORT
void nvnc_h264_encoder_set_userdata(struct nvnc_h264_encoder* self,
		void* userdata)
{
	self->userdata = userdata;
}

EXPORT
void* nvnc_h264_encoder_get_userdata(const struct nvnc_h264_encoder* self)
{
	return self->userdata;
}

EXPORT
void nvnc_h264_encoder_packet_ready(struct nvnc_h264_encoder* self,
		const void* data, size_t size, uint64_t pts)
{
	self->base.on_packet_ready(data, size, pts, self->base.userdata);
}

struct h264_encoder_impl h264_encoder_plugin_impl = {
	.destroy = h264_encoder_plugin_destroy,
	.feed = h264_encoder_plugin_feed,
};
