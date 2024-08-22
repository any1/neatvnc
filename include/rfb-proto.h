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
	RFB_SECURITY_TYPE_RSA_AES = 5,
	RFB_SECURITY_TYPE_TIGHT = 16,
	RFB_SECURITY_TYPE_VENCRYPT = 19,
	RFB_SECURITY_TYPE_APPLE_DH = 30,
	RFB_SECURITY_TYPE_RSA_AES256 = 129,
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
	RFB_CLIENT_TO_SERVER_NTP = 160,
	RFB_CLIENT_TO_SERVER_SET_DESKTOP_SIZE = 251,
	RFB_CLIENT_TO_SERVER_QEMU = 255,
};

enum rfb_client_to_server_qemu_msg_type {
	RFB_CLIENT_TO_SERVER_QEMU_KEY_EVENT = 0,
};

enum rfb_encodings {
	RFB_ENCODING_RAW = 0,
	RFB_ENCODING_COPYRECT = 1,
	RFB_ENCODING_RRE = 2,
	RFB_ENCODING_HEXTILE = 5,
	RFB_ENCODING_TIGHT = 7,
	RFB_ENCODING_TRLE = 15,
	RFB_ENCODING_ZRLE = 16,
	RFB_ENCODING_OPEN_H264 = 50,
	RFB_ENCODING_CURSOR = -239,
	RFB_ENCODING_DESKTOPSIZE = -223,
	RFB_ENCODING_QEMU_EXT_KEY_EVENT = -258,
	RFB_ENCODING_QEMU_LED_STATE = -261,
	RFB_ENCODING_EXTENDEDDESKTOPSIZE = -308,
	RFB_ENCODING_PTS = -1000,
	RFB_ENCODING_NTP = -1001,
	RFB_ENCODING_VMWARE_LED_STATE = 0x574d5668,
	// 0xc0a1e5ce, greater than INT_MAX
	RFB_ENCODING_EXTENDED_CLIPBOARD = -1063131698,
};

#define RFB_ENCODING_JPEG_HIGHQ -23
#define RFB_ENCODING_JPEG_LOWQ -32

enum rfb_server_to_client_msg_type {
	RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE = 0,
	RFB_SERVER_TO_CLIENT_SET_COLOUR_MAP_ENTRIES = 1,
	RFB_SERVER_TO_CLIENT_BELL = 2,
	RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT = 3,
	RFB_SERVER_TO_CLIENT_NTP = 160,
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

enum rfb_resize_initiator {
	RFB_RESIZE_INITIATOR_SERVER = 0,
	RFB_RESIZE_INITIATOR_THIS_CLIENT = 1,
	RFB_RESIZE_INITIATOR_OTHER_CLIENT = 2,
};

enum rfb_resize_status {
	RFB_RESIZE_STATUS_SUCCESS = 0,
	RFB_RESIZE_STATUS_PROHIBITED = 1,
	RFB_RESIZE_STATUS_OUT_OF_RESOURCES = 2,
	RFB_RESIZE_STATUS_INVALID_LAYOUT = 3,
	RFB_RESIZE_STATUS_REQUEST_FORWARDED = 4,
};

enum rfb_rsa_aes_cred_subtype {
	RFB_RSA_AES_CRED_SUBTYPE_USER_AND_PASS = 1,
	RFB_RSA_AES_CRED_SUBTYPE_ONLY_PASS = 2,
};

// This is the same for both qemu and vmware extensions
enum rfb_led_state {
	RFB_LED_STATE_SCROLL_LOCK = 1 << 0,
	RFB_LED_STATE_NUM_LOCK = 1 << 1,
	RFB_LED_STATE_CAPS_LOCK = 1 << 2,
};

enum rfb_ext_clipboard_flags {
	RFB_EXT_CLIPBOARD_FORMAT_TEXT = 1 << 0,
	RFB_EXT_CLIPBOARD_FORMAT_RTF = 1 << 1,
	RFB_EXT_CLIPBOARD_FORMAT_HTML = 1 << 2,
	RFB_EXT_CLIPBOARD_FORMAT_DIB = 1 << 3,
	RFB_EXT_CLIPBOARD_FORMAT_FILES = 1 << 4,
	RFB_EXT_CLIPBOARD_CAPS = 1 << 24,
	RFB_EXT_CLIPBOARD_ACTION_REQUEST = 1 << 25,
	RFB_EXT_CLIPBOARD_ACTION_PEEK = 1 << 26,
	RFB_EXT_CLIPBOARD_ACTION_NOTIFY = 1 << 27,
	RFB_EXT_CLIPBOARD_ACTION_PROVIDE = 1 << 28,
	RFB_EXT_CLIPBOARD_ACTION_ALL =
		RFB_EXT_CLIPBOARD_ACTION_REQUEST |
		RFB_EXT_CLIPBOARD_ACTION_PEEK |
		RFB_EXT_CLIPBOARD_ACTION_NOTIFY |
		RFB_EXT_CLIPBOARD_ACTION_PROVIDE,
};

struct rfb_security_types_msg {
	uint8_t n;
	uint8_t types[0];
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

struct rfb_client_qemu_key_event_msg {
	uint8_t type;
	uint8_t subtype;
	uint16_t down_flag;
	uint32_t keysym;
	uint32_t keycode;
} RFB_PACKED;

struct rfb_client_pointer_event_msg {
	uint8_t type;
	uint8_t button_mask;
	uint16_t x;
	uint16_t y;
} RFB_PACKED;

struct rfb_ext_clipboard_msg {
	uint8_t type;
	uint8_t padding[3];
	uint32_t length;
	uint32_t flags;
	union {
		uint32_t max_unsolicited_sizes[0];
		unsigned char zlib_stream[0];
	};
} RFB_PACKED;

struct rfb_cut_text_msg {
	uint8_t type;
	uint8_t padding[3];
	uint32_t length;
	char text[0];
} RFB_PACKED;

struct rfb_server_fb_rect {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	int32_t encoding;
} RFB_PACKED;

struct rfb_screen {
	uint32_t id;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	uint32_t flags;
} RFB_PACKED;

struct rfb_client_set_desktop_size_event_msg {
	uint8_t type;
	uint8_t padding;
	uint16_t width;
	uint16_t height;
	uint8_t number_of_screens;
	uint8_t padding2;
	struct rfb_screen screens[0];
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

struct rfb_ntp_msg {
	uint8_t type;
	uint8_t padding[3];
	uint32_t t0, t1, t2, t3;
} RFB_PACKED;

struct rfb_apple_dh_server_msg {
	uint16_t generator;
	uint16_t key_size;
	uint8_t modulus_and_key[0];
} RFB_PACKED;

struct rfb_apple_dh_client_msg {
	uint8_t encrypted_credentials[128];
	uint8_t public_key[0];
} RFB_PACKED;

struct rfb_rsa_aes_pub_key_msg {
	uint32_t length;
	uint8_t modulus_and_exponent[0];
} RFB_PACKED;

struct rfb_rsa_aes_challenge_msg {
	uint16_t length;
	uint8_t challenge[0];
} RFB_PACKED;

struct rfb_colour_map_entry {
	uint16_t r, g, b;
} RFB_PACKED;

struct rfb_set_colour_map_entries_msg {
	uint8_t type;
	uint8_t padding;
	uint16_t first_colour;
	uint16_t n_colours;
	struct rfb_colour_map_entry colours[0];
} RFB_PACKED;
