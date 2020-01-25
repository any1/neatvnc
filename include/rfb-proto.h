/*
 * Copyright (c) 2019 Andri Yngvason
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

#define RFB_VERSION_MESSAGE "RFB 003.008\n"

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#define RFB_PACKED __attribute__((packed))

enum rfb_security_type {
	RFB_SECURITY_TYPE_INVALID = 0,
	RFB_SECURITY_TYPE_NONE = 1,
	RFB_SECURITY_TYPE_VNC_AUTH = 2,
	RFB_SECURITY_TYPE_TIGHT = 16,
	RFB_SECURITY_TYPE_VENCRYPT = 19,
};

enum rfb_security_handshake_result {
	RFB_SECURITY_HANDSHAKE_OK = 0,
	RFB_SECURITY_HANDSHAKE_FAILED = 1,
};

enum rfb_client_to_server_msg_type {
	RFB_CLIENT_TO_SERVER_SET_PIXEL_FORMAT = 0,
	RFB_CLIENT_TO_SERVER_SET_ENCODINGS = 2,
	RFB_CLIENT_TO_SERVER_FRAMEBUFFER_UPDATE_REQUEST = 3,
	RFB_CLIENT_TO_SERVER_KEY_EVENT = 4,
	RFB_CLIENT_TO_SERVER_POINTER_EVENT = 5,
	RFB_CLIENT_TO_SERVER_CLIENT_CUT_TEXT = 6,
};

enum rfb_encodings {
	RFB_ENCODING_RAW = 0,
	RFB_ENCODING_COPYRECT = 1,
	RFB_ENCODING_RRE = 2,
	RFB_ENCODING_HEXTILE = 5,
	RFB_ENCODING_TIGHT = 7,
	RFB_ENCODING_TRLE = 15,
	RFB_ENCODING_ZRLE = 16,
	RFB_ENCODING_CURSOR = -239,
	RFB_ENCODING_DESKTOPSIZE = -223,
};

enum rfb_server_to_client_msg_type {
	RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE = 0,
	RFB_SERVER_TO_CLIENT_SET_COLOUR_MAP_ENTRIES = 1,
	RFB_SERVER_TO_CLIENT_BELL = 2,
	RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT = 3,
};

enum rfb_vencrypt_subtype {
	RFB_VENCRYPT_PLAIN = 256,
	RFB_VENCRYPT_TLS_NONE,
	RFB_VENCRYPT_TLS_VNC,
	RFB_VENCRYPT_TLS_PLAIN,
	RFB_VENCRYPT_X509_NONE,
	RFB_VENCRYPT_X509_VNC,
	RFB_VENCRYPT_X509_PLAIN,
};

struct rfb_security_types_msg {
	uint8_t n;
	uint8_t types[1];
} RFB_PACKED;

struct rfb_error_reason {
	uint32_t length;
	char message[0];
} RFB_PACKED;

struct rfb_pixel_format {
	uint8_t bits_per_pixel;
	uint8_t depth;
	uint8_t big_endian_flag;
	uint8_t true_colour_flag;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
	uint8_t padding[3];
} RFB_PACKED;

struct rfb_server_init_msg {
	uint16_t width;
	uint16_t height;
	struct rfb_pixel_format pixel_format;
	uint32_t name_length;
	char name_string[0];
} RFB_PACKED;

struct rfb_client_set_encodings_msg {
	uint8_t type;
	uint8_t padding;
	uint16_t n_encodings;
	int32_t encodings[0];
} RFB_PACKED;

struct rfb_client_fb_update_req_msg {
	uint8_t type;
	uint8_t incremental;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
} RFB_PACKED;

struct rfb_client_key_event_msg {
	uint8_t type;
	uint8_t down_flag;
	uint16_t padding;
	uint32_t key;
} RFB_PACKED;

struct rfb_client_pointer_event_msg {
	uint8_t type;
	uint8_t button_mask;
	uint16_t x;
	uint16_t y;
} RFB_PACKED;

struct rfb_client_cut_text_msg {
	uint8_t type;
	uint8_t padding[3];
	uint32_t length;
	char test[0];
} RFB_PACKED;

struct rfb_server_fb_rect {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	int32_t encoding;
} RFB_PACKED;

struct rfb_server_fb_update_msg {
	uint8_t type;
	uint8_t padding;
	uint16_t n_rects;
} RFB_PACKED;

struct rfb_vencrypt_version_msg {
	uint8_t major;
	uint8_t minor;
} RFB_PACKED;

struct rfb_vencrypt_subtypes_msg {
	uint8_t n;
	uint32_t types[1];
} RFB_PACKED;

struct rfb_vencrypt_plain_auth_msg {
	uint32_t username_len;
	uint32_t password_len;
	char text[0];
} RFB_PACKED;
