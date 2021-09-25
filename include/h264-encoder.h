#pragma once

#include <stdint.h>
#include <unistd.h>

struct h264_encoder;
struct nvnc_fb;

typedef void (*h264_encoder_packet_handler_fn)(const void* payload, size_t size,
		void* userdata);

struct h264_encoder* h264_encoder_create(uint32_t width, uint32_t height,
		uint32_t format);

void h264_encoder_destroy(struct h264_encoder*);

void h264_encoder_set_packet_handler_fn(struct h264_encoder*,
		h264_encoder_packet_handler_fn);
void h264_encoder_set_userdata(struct h264_encoder*, void* userdata);

void h264_encoder_feed(struct h264_encoder*, struct nvnc_fb*);

void h264_encoder_request_keyframe(struct h264_encoder*);
