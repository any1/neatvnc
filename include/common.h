/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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

#include <stdbool.h>
#include <pixman.h>
#include <zlib.h>

#include "rfb-proto.h"
#include "sys/queue.h"

#include "neatvnc.h"
#include "config.h"

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

#define MAX_ENCODINGS 32
#define MAX_OUTGOING_FRAMES 4
#define MSG_BUFFER_SIZE 4096
#define MAX_CUT_TEXT_SIZE 10000000

enum nvnc_client_state {
	VNC_CLIENT_STATE_ERROR = -1,
	VNC_CLIENT_STATE_WAITING_FOR_VERSION = 0,
	VNC_CLIENT_STATE_WAITING_FOR_SECURITY,
#ifdef ENABLE_TLS
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH,
#endif
	VNC_CLIENT_STATE_WAITING_FOR_INIT,
	VNC_CLIENT_STATE_READY,
};

struct nvnc;
struct stream;
struct aml_handler;
struct aml_idle;
struct nvnc_display;

struct nvnc_common {
	void* userdata;
	nvnc_cleanup_fn cleanup_fn;
};

struct cut_text {
	char* buffer;
	size_t length;
	size_t index;
};

struct nvnc_client {
	struct nvnc_common common;
	int ref;
	struct stream* net_stream;
	char hostname[256];
	char username[256];
	struct nvnc* server;
	enum nvnc_client_state state;
	bool has_pixfmt;
	struct rfb_pixel_format pixfmt;
	enum rfb_encodings encodings[MAX_ENCODINGS + 1];
	size_t n_encodings;
	LIST_ENTRY(nvnc_client) link;
	struct pixman_region16 damage;
	int n_pending_requests;
	bool is_updating;
	struct nvnc_fb* current_fb;
	nvnc_client_fn cleanup_fn;
	size_t buffer_index;
	size_t buffer_len;
	uint8_t msg_buffer[MSG_BUFFER_SIZE];
	uint32_t known_width;
	uint32_t known_height;
	struct cut_text cut_text;
	bool is_qemu_key_ext_notified;
	struct encoder* encoder;
	uint32_t cursor_seq;
	int quality;
};

LIST_HEAD(nvnc_client_list, nvnc_client);

struct nvnc {
	struct nvnc_common common;
	int fd;
	struct aml_handler* poll_handle;
	struct nvnc_client_list clients;
	char name[256];
	void* userdata;
	nvnc_key_fn key_fn;
	nvnc_key_fn key_code_fn;
	nvnc_pointer_fn pointer_fn;
	nvnc_fb_req_fn fb_req_fn;
	nvnc_client_fn new_client_fn;
	nvnc_cut_text_fn cut_text_fn;
	struct nvnc_display* display;
	struct {
		struct nvnc_fb* buffer;
		uint32_t width, height;
		uint32_t hotspot_x, hotspot_y;
	} cursor;
	uint32_t cursor_seq;

#ifdef ENABLE_TLS
	gnutls_certificate_credentials_t tls_creds;
	nvnc_auth_fn auth_fn;
	void* auth_ud;
#endif

	uint32_t n_damage_clients;
};

void nvnc__damage_region(struct nvnc* self,
                         const struct pixman_region16* damage);
