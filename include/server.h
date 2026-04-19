/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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

#include "client.h"
#include "common.h"
#include "config.h"
#include "cut-text.h"
#include "frame.h"
#include "neatvnc.h"
#include "rfb-proto.h"
#include "sys/queue.h"
#include "weakref.h"

#ifdef HAVE_CRYPTO
#include "crypto.h"
#endif

#include <pixman.h>
#include <stdbool.h>
#include <zlib.h>

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

#define MAX_OUTGOING_FRAMES 4
#define MAX_CUT_TEXT_SIZE 10000000
#define MAX_CLIENT_UNSOLICITED_TEXT_SIZE 20971520
#define MAX_SECURITY_TYPES 32

struct aml_handler;
struct crypto_rsa_priv_key;
struct crypto_rsa_pub_key;
struct nvnc;
struct nvnc_display;

enum nvnc__socket_type {
	NVNC__SOCKET_TCP,
	NVNC__SOCKET_UNIX,
	NVNC__SOCKET_WEBSOCKET,
	NVNC__SOCKET_FROM_FD,
};

struct nvnc__socket {
	struct nvnc* parent;
	enum nvnc_stream_type type;
	bool is_external;
	int fd;
	struct aml_handler* poll_handle;
	LIST_ENTRY(nvnc__socket) link;
};

LIST_HEAD(nvnc__socket_list, nvnc__socket);

struct nvnc {
	struct nvnc_common common;
	bool is_closing;
	struct nvnc__socket_list sockets;
	struct nvnc_client_list clients;
	char name[256];
	void* userdata;
	nvnc_key_fn key_fn;
	nvnc_key_fn key_code_fn;
	nvnc_pointer_fn pointer_fn;
	nvnc_normalised_pointer_fn normalised_pointer_fn;
	nvnc_client_fn new_client_fn;
	nvnc_cut_text_fn cut_text_fn;
	struct cut_text ext_clipboard_provide_msg;
	nvnc_desktop_layout_fn desktop_layout_fn;
	int n_displays;
	struct nvnc_display* displays[NVNC_FB_COMPOSITE_MAX];
	uint32_t next_display_id;
	struct {
		struct nvnc_frame* buffer;
		uint32_t hotspot_x, hotspot_y;
	} cursor;
	uint32_t cursor_seq;

	enum nvnc_auth_flags auth_flags;
	nvnc_auth_fn auth_fn;
	void* auth_ud;

#ifdef ENABLE_TLS
	gnutls_certificate_credentials_t tls_creds;
#endif

#ifdef HAVE_CRYPTO
	struct crypto_rsa_pub_key* rsa_pub;
	struct crypto_rsa_priv_key* rsa_priv;
#endif

	int n_security_types;
	enum rfb_security_type security_types[MAX_SECURITY_TYPES];

	uint32_t n_damage_clients;
};

void nvnc__damage_region(struct nvnc* self,
		const struct pixman_region16* damage);
void update_min_rtt(struct nvnc_client* client);
void nvnc__reset_encoders(struct nvnc* self);
