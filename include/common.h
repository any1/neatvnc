#pragma once

#include <uv.h>
#include <stdbool.h>
#include <pixman.h>

#include "rfb-proto.h"
#include "sys/queue.h"

#include "neatvnc.h"
#include "miniz.h"

#define MAX_ENCODINGS 32
#define MAX_OUTGOING_FRAMES 4
#define MSG_BUFFER_SIZE 4096

enum nvnc_client_state {
	VNC_CLIENT_STATE_ERROR = -1,
	VNC_CLIENT_STATE_WAITING_FOR_VERSION = 0,
	VNC_CLIENT_STATE_WAITING_FOR_SECURITY,
	VNC_CLIENT_STATE_WAITING_FOR_INIT,
	VNC_CLIENT_STATE_READY,
};

struct nvnc;

struct nvnc_common {
	void* userdata;
};

struct nvnc_client {
	struct nvnc_common common;
	int ref;
	struct uv_tcp_s stream_handle;
	struct nvnc* server;
	enum nvnc_client_state state;
	uint32_t fourcc;
	struct rfb_pixel_format pixfmt;
	enum rfb_encodings encodings[MAX_ENCODINGS + 1];
	size_t n_encodings;
	LIST_ENTRY(nvnc_client) link;
	struct pixman_region16 damage;
	int n_pending_requests;
	bool is_updating;
	nvnc_client_fn cleanup_fn;
	z_stream z_stream;
	size_t buffer_index;
	size_t buffer_len;
	uint8_t msg_buffer[MSG_BUFFER_SIZE];
};

LIST_HEAD(nvnc_client_list, nvnc_client);

struct vnc_display {
	uint16_t width;
	uint16_t height;
	uint32_t pixfmt; /* fourcc pixel format */
	char name[256];
};

struct nvnc {
	struct nvnc_common common;
	uv_tcp_t tcp_handle;
	struct nvnc_client_list clients;
	struct vnc_display display;
	void* userdata;
	nvnc_key_fn key_fn;
	nvnc_pointer_fn pointer_fn;
	nvnc_fb_req_fn fb_req_fn;
	nvnc_client_fn new_client_fn;
	struct nvnc_fb* frame;
};
