/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
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

#include "stream/stream.h"
#include "neatvnc.h"
#include "config.h"

#ifdef HAVE_CRYPTO
#include "crypto.h"
#endif

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

#define MAX_ENCODINGS 32
#define MAX_OUTGOING_FRAMES 4
#define MSG_BUFFER_SIZE 4096
#define MAX_CUT_TEXT_SIZE 10000000
#define MAX_CLIENT_UNSOLICITED_TEXT_SIZE 20971520
#define MAX_SECURITY_TYPES 32

enum nvnc_client_state {
	VNC_CLIENT_STATE_WAITING_FOR_VERSION = 0,
	VNC_CLIENT_STATE_WAITING_FOR_SECURITY,
#ifdef ENABLE_TLS
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE,
	VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH,
#endif
#ifdef HAVE_VNC_AUTH
	VNC_CLIENT_STATE_WAITING_FOR_VNC_AUTH_RESPONSE,
#endif
#ifdef HAVE_CRYPTO
	VNC_CLIENT_STATE_WAITING_FOR_APPLE_DH_RESPONSE,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CHALLENGE,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CLIENT_HASH,
	VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CREDENTIALS,
#endif
	VNC_CLIENT_STATE_WAITING_FOR_INIT,
	VNC_CLIENT_STATE_READY,
};

#define VNC_AUTH_CHALLENGE_LEN 16
#define VNC_AUTH_PASSWORD_LEN 8
#define VNC_AUTH_RESPONSE_LEN VNC_AUTH_CHALLENGE_LEN

struct nvnc;
struct stream;
struct aml_handler;
struct aml_idle;
struct nvnc_display;
struct crypto_key;
struct crypto_rsa_pub_key;
struct crypto_rsa_priv_key;
struct bwe;

struct nvnc_common {
	void* userdata;
	nvnc_cleanup_fn cleanup_fn;
};

struct cut_text {
	char* buffer;
	size_t length;
	size_t index;
	bool is_zlib;
	bool is_text_provide;
};

struct nvnc_client {
	struct nvnc_common common;
	struct stream* net_stream;
	char username[256];
	struct nvnc* server;
	enum nvnc_client_state state;
	struct rfb_pixel_format pixfmt;
	enum rfb_encodings encodings[MAX_ENCODINGS + 1];
	size_t n_encodings;
	LIST_ENTRY(nvnc_client) link;
	struct pixman_region16 damage;
	int n_pending_requests;
	bool is_updating;
	bool rfb_less_38;
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
	uint32_t cursor_seq;
	int quality;
	bool formats_changed;
	enum nvnc_keyboard_led_state led_state;
	enum nvnc_keyboard_led_state pending_led_state;
	bool is_blocked_by_fence;
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

#ifdef HAVE_VNC_AUTH
	uint8_t vnc_auth_challenge[VNC_AUTH_CHALLENGE_LEN];
#endif
#ifdef HAVE_CRYPTO
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
	nvnc_fb_req_fn fb_req_fn;
	nvnc_client_fn new_client_fn;
	nvnc_cut_text_fn cut_text_fn;
	struct cut_text ext_clipboard_provide_msg;
	nvnc_desktop_layout_fn desktop_layout_fn;
	struct nvnc_display* display;
	struct {
		struct nvnc_fb* buffer;
		uint32_t width, height;
		uint32_t hotspot_x, hotspot_y;
	} cursor;
	uint32_t cursor_seq;

	enum nvnc_auth_flags auth_flags;
	nvnc_auth_fn auth_fn;
	void* auth_ud;

#ifdef ENABLE_TLS
	gnutls_certificate_credentials_t tls_creds;
#endif

#ifdef HAVE_VNC_AUTH
#endif
	uint8_t vnc_auth_password[VNC_AUTH_PASSWORD_LEN];
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
void close_after_write(void* userdata, enum stream_req_status status);
void update_min_rtt(struct nvnc_client* client);
