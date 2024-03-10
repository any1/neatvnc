/*
 * Copyright (c) 2021 - 2024 Andri Yngvason
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
#include <unistd.h>
#include <stdbool.h>

struct nvnc_fb;
struct h264_encoder;

typedef void (*h264_encoder_packet_handler_fn)(const void* payload, size_t size,
		uint64_t pts, void* userdata);

struct h264_encoder_impl {
	struct h264_encoder* (*create)(uint32_t width, uint32_t height,
			uint32_t format, int quality);
	void (*destroy)(struct h264_encoder*);
	void (*feed)(struct h264_encoder*, struct nvnc_fb*);
};

struct h264_encoder {
	struct h264_encoder_impl *impl;
	h264_encoder_packet_handler_fn on_packet_ready;
	void* userdata;
	bool next_frame_should_be_keyframe;
};

struct h264_encoder* h264_encoder_create(uint32_t width, uint32_t height,
		uint32_t format, int quality);

void h264_encoder_destroy(struct h264_encoder*);

void h264_encoder_set_packet_handler_fn(struct h264_encoder*,
		h264_encoder_packet_handler_fn);
void h264_encoder_set_userdata(struct h264_encoder*, void* userdata);

void h264_encoder_feed(struct h264_encoder*, struct nvnc_fb*);

void h264_encoder_request_keyframe(struct h264_encoder*);
