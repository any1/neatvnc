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

#include "common.h"
#include "config.h"
#include "cut-text.h"
#include "rfb-proto.h"
#include "weakref.h"

#ifdef HAVE_CRYPTO
#include "crypto.h"
#endif

#include <pixman.h>

#define MAX_ENCODINGS 32
#define MSG_BUFFER_SIZE 4096

struct aml_idle;
struct bwe;
struct compositor;
struct crypto_key;
struct encoder;
struct nvnc;
struct stream;

enum nvnc_client_state {
	VNC_CLIENT_STATE_WAITING_FOR_VERSION = 0,
	VNC_CLIENT_STATE_WAITING_FOR_SECURITY,
#ifdef ENABLE_TLS
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH,
#endif
#ifdef HAVE_CRYPTO
	VNC_CLIENT_STATE_WAITING_FOR_DES_AUTH_RESPONSE,
	VNC_CLIENT_STATE_WAITING_FOR_APPLE_DH_RESPONSE,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CHALLENGE,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CLIENT_HASH,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CREDENTIALS,
#endif
	VNC_CLIENT_STATE_WAITING_FOR_INIT,
	VNC_CLIENT_STATE_WAITING_FOR_AUTH,
	VNC_CLIENT_STATE_READY,
};

struct nvnc_client {
	struct nvnc_common common;
	struct weakref_subject weakref;
	struct stream* net_stream;
	char username[256];
	struct nvnc* server;
	enum nvnc_client_state state;
	uint16_t rfb_minor_version;
	struct rfb_pixel_format pixfmt;
	enum rfb_encodings encodings[MAX_ENCODINGS + 1];
	size_t n_encodings;
	LIST_ENTRY(nvnc_client) link;
	struct pixman_region16 damage;
	int n_pending_requests;
	bool is_updating;
	nvnc_client_fn cleanup_fn;
	size_t buffer_index;
	size_t buffer_len;
	uint8_t msg_buffer[MSG_BUFFER_SIZE];
	uint32_t known_width;
	uint32_t known_height;
	struct cut_text cut_text;
	uint32_t ext_clipboard_caps;
	uint32_t ext_clipboard_max_unsolicited_text_size;
	bool is_ext_notified;
	bool is_continuous_updates_notified;
	bool continuous_updates_enabled;
	struct {
		int x, y;
		unsigned int width, height;
	} continuous_updates;
	struct encoder* encoder;
	struct encoder* zrle_encoder;
	struct encoder* tight_encoder;
	struct compositor* compositor;
	uint32_t cursor_seq;
	int quality;
	bool formats_changed;
	enum nvnc_keyboard_led_state led_state;
	enum nvnc_keyboard_led_state pending_led_state;
	bool is_blocked_by_fence;
	bool is_processing_messages;
	bool must_block_after_next_message;
	struct {
		int n_pending_requests;
		enum rfb_fence_flags flags;
		uint8_t payload[64];
		size_t length;
	} pending_fence;
	int32_t last_ping_time;
	int32_t min_rtt;
	struct bwe* bwe;
	int32_t inflight_bytes;
	bool has_ext_mouse_buttons;
	struct aml_idle* close_task;
	bool needs_desktop_name_update;

#ifdef HAVE_CRYPTO
	uint8_t des_challenge[16];
	struct crypto_key* apple_dh_secret;

	struct {
		enum crypto_hash_type hash_type;
		enum crypto_cipher_type cipher_type;
		size_t challenge_len;
		uint8_t challenge[32];
		struct crypto_rsa_pub_key* pub;
	} rsa;
#endif
};

LIST_HEAD(nvnc_client_list, nvnc_client);

