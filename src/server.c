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

#include "rfb-proto.h"
#include "vec.h"
#include "type-macros.h"
#include "fb.h"
#include "desktop-layout.h"
#include "display.h"
#include "neatvnc.h"
#include "common.h"
#include "pixels.h"
#include "stream/stream.h"
#include "config.h"
#include "usdt.h"
#include "enc/encoder.h"
#include "enc/util.h"
#include "enc/h264-encoder.h"
#include "cursor.h"
#include "logging.h"
#include "auth/auth.h"
#include "bandwidth.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <assert.h>
#include <aml.h>
#include <libdrm/drm_fourcc.h>
#include <pixman.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <zlib.h>
#include <tgmath.h>

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#include "auth/vencrypt.h"
#endif

#ifdef HAVE_CRYPTO
#include "crypto.h"
#include "auth/apple-dh.h"
#include "auth/rsa-aes.h"
#endif

#ifndef DRM_FORMAT_INVALID
#define DRM_FORMAT_INVALID 0
#endif

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR DRM_FORMAT_MOD_NONE
#endif

#define DEFAULT_NAME "Neat VNC"

#define EXPORT __attribute__((visibility("default")))

static int send_desktop_resize_rect(struct nvnc_client* client, uint16_t width,
		uint16_t height);
static bool client_supports_resizing(const struct nvnc_client* client);
static bool send_ext_support_frame(struct nvnc_client* client);
static void send_ext_clipboard_caps(struct nvnc_client* client);
static enum rfb_encodings choose_frame_encoding(struct nvnc_client* client,
		const struct nvnc_fb*);
static void on_encode_frame_done(struct encoder*, struct encoded_frame*);
static bool client_has_encoding(const struct nvnc_client* client,
		enum rfb_encodings encoding);
static void process_fb_update_requests(struct nvnc_client* client);
static void sockaddr_to_string(char* dst, size_t sz,
		const struct sockaddr* addr);
static const char* encoding_to_string(enum rfb_encodings encoding);
static bool client_send_led_state(struct nvnc_client* client);
static void process_pending_fence(struct nvnc_client* client);

#if defined(PROJECT_VERSION)
EXPORT const char nvnc_version[] = PROJECT_VERSION;
#else
EXPORT const char nvnc_version[] = "UNKNOWN";
#endif

extern const unsigned short code_map_qnum_to_linux[];
extern const unsigned int code_map_qnum_to_linux_len;

static uint64_t nvnc__htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(x);
#else
	return x;
#endif
}

static uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static bool have_working_h264_encoder(void)
{
	static int cached_result;

	if (cached_result) {
		return cached_result == 1;
	}

	struct h264_encoder *encoder = h264_encoder_create(1920, 1080,
			DRM_FORMAT_XRGB8888, 5);
	cached_result = encoder ? 1 : -1;
	h264_encoder_destroy(encoder);

	nvnc_log(NVNC_LOG_DEBUG, "H.264 encoding is %s",
			cached_result == 1 ? "available" : "unavailable");

	return cached_result == 1;
}

static void client_drain_encoder(struct nvnc_client* client)
{
	 /* Letting the encoder finish is the simplest way to free its
	  * in-flight resources.
	  */
	int64_t timeout = 1000000; // µs
	int64_t remaining = timeout;
	int64_t start_time = gettime_us(CLOCK_MONOTONIC);

	while (client->is_updating) {
		aml_poll(aml_get_default(), remaining);
		aml_dispatch(aml_get_default());

		int64_t now = gettime_us(CLOCK_MONOTONIC);
		int64_t dt = now - start_time;
		remaining = timeout - dt;
		if (remaining <= 0) {
			nvnc_log(NVNC_LOG_PANIC, "Encoder stalled while closing");
			break;
		}
	}
}

static void client_close(struct nvnc_client* client)
{
	if (client->close_task) {
		struct aml_idle* task = client->close_task;
		client->close_task = NULL;
		aml_stop(aml_get_default(), task);
		aml_unref(task);
	}

	nvnc_log(NVNC_LOG_INFO, "Closing client connection %p", client);

	stream_close(client->net_stream);

	if (client->server->is_closing)
		client_drain_encoder(client);

	nvnc_cleanup_fn cleanup = client->common.cleanup_fn;
	if (cleanup)
		cleanup(client->common.userdata);

	nvnc_client_fn fn = client->cleanup_fn;
	if (fn)
		fn(client);

	bwe_destroy(client->bwe);

#ifdef HAVE_CRYPTO
	crypto_key_del(client->apple_dh_secret);
	crypto_rsa_pub_key_del(client->rsa.pub);
#endif

	LIST_REMOVE(client, link);
	stream_destroy(client->net_stream);
	if (client->encoder) {
		client->server->n_damage_clients -=
			!(client->encoder->impl->flags &
					ENCODER_IMPL_FLAG_IGNORES_DAMAGE);
		client->encoder->on_done = NULL;
		client->encoder->userdata = NULL;
	}
	encoder_unref(client->encoder);
	encoder_unref(client->zrle_encoder);
	encoder_unref(client->tight_encoder);
	pixman_region_fini(&client->damage);
	free(client->cut_text.buffer);
	free(client);
}

static void do_deferred_client_close(struct aml_idle* idle)
{
	struct nvnc_client* client = aml_get_userdata(idle);
	client->close_task = NULL;
	aml_stop(aml_get_default(), idle);
	aml_unref(idle);

	client_close(client);
}

static void defer_client_close(struct nvnc_client* client)
{
	if (client->close_task)
		return;
	client->close_task = aml_idle_new(do_deferred_client_close, client,
			NULL);
	aml_start(aml_get_default(), client->close_task);
}

void close_after_write(void* userdata, enum stream_req_status status)
{
	struct stream* stream = userdata;
	stream_destroy(stream);
}

static int handle_unsupported_version(struct nvnc_client* client)
{
	char buffer[256];

	struct rfb_error_reason* reason = (struct rfb_error_reason*)(buffer + 1);

	static const char reason_string[] = "Unsupported version\n";

	buffer[0] = 0; /* Number of security types is 0 on error */
	reason->length = htonl(strlen(reason_string));
	strcpy(reason->message, reason_string);

	size_t len = 1 + sizeof(*reason) + strlen(reason_string);
	stream_write(client->net_stream, buffer, len, close_after_write,
			client->net_stream);

	// Keep stream alive until the result has been sent to the client
	stream_ref(client->net_stream);

	client_close(client);
	return -1;
}

static void init_security_types(struct nvnc* server)
{
#define ADD_SECURITY_TYPE(type) \
	assert(server->n_security_types < MAX_SECURITY_TYPES); \
	server->security_types[server->n_security_types++] = (type);

	if (server->n_security_types > 0)
		return;

	if (server->auth_flags & NVNC_AUTH_REQUIRE_AUTH) {
		assert(server->auth_fn);

#ifdef ENABLE_TLS
		if (server->tls_creds) {
			ADD_SECURITY_TYPE(RFB_SECURITY_TYPE_VENCRYPT);
		}
#endif

#ifdef HAVE_CRYPTO
		ADD_SECURITY_TYPE(RFB_SECURITY_TYPE_RSA_AES256);
		ADD_SECURITY_TYPE(RFB_SECURITY_TYPE_RSA_AES);

		if (!(server->auth_flags & NVNC_AUTH_REQUIRE_ENCRYPTION)) {
			ADD_SECURITY_TYPE(RFB_SECURITY_TYPE_APPLE_DH);
		}
#endif
	} else {
		ADD_SECURITY_TYPE(RFB_SECURITY_TYPE_NONE);
	}

	if (server->n_security_types == 0) {
		nvnc_log(NVNC_LOG_PANIC, "Failed to satisfy requested security constraints");
	}

#undef ADD_SECURITY_TYPE
}

static bool is_allowed_security_type(const struct nvnc* server, uint8_t type)
{
	for (int i = 0; i < server->n_security_types; ++i) {
		if ((uint8_t)server->security_types[i] == type) {
			return true;
		}
	}
	return false;
}

void update_min_rtt(struct nvnc_client* client)
{
	int32_t now = gettime_us(CLOCK_MONOTONIC);
	int32_t diff = now - client->last_ping_time;
	client->last_ping_time = now;

	if (diff < client->min_rtt) {
		client->min_rtt = diff;
		bwe_update_rtt_min(client->bwe, diff);
	}
}

static int on_version_message(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (client->buffer_len - client->buffer_index < 12)
		return 0;

	char version_string[13];
	memcpy(version_string, client->msg_buffer + client->buffer_index, 12);
	version_string[12] = '\0';

	if (strcmp(RFB_VERSION_MESSAGE, version_string) != 0)
		return handle_unsupported_version(client);

	uint8_t buf[sizeof(struct rfb_security_types_msg) +
		MAX_SECURITY_TYPES] = {};
	struct rfb_security_types_msg* security =
		(struct rfb_security_types_msg*)buf;

	init_security_types(server);

	security->n = server->n_security_types;
	for (int i = 0; i < server->n_security_types; ++i) {
		security->types[i] = server->security_types[i];
	}

	update_min_rtt(client);

	stream_write(client->net_stream, security, sizeof(*security) +
			security->n, NULL, NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_SECURITY;
	return 12;
}

static int on_security_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t type = client->msg_buffer[client->buffer_index];
	nvnc_log(NVNC_LOG_DEBUG, "Client chose security type: %d", type);

	if (!is_allowed_security_type(client->server, type)) {
		security_handshake_failed(client, NULL, "Illegal security type");
		return -1;
	}

	update_min_rtt(client);

	switch (type) {
	case RFB_SECURITY_TYPE_NONE:
		security_handshake_ok(client, NULL);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
		break;
#ifdef ENABLE_TLS
	case RFB_SECURITY_TYPE_VENCRYPT:
		vencrypt_send_version(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION;
		break;
#endif
#ifdef HAVE_CRYPTO
	case RFB_SECURITY_TYPE_APPLE_DH:
		apple_dh_send_public_key(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_APPLE_DH_RESPONSE;
		break;
	case RFB_SECURITY_TYPE_RSA_AES:
		client->rsa.hash_type = CRYPTO_HASH_SHA1;
		client->rsa.cipher_type = CRYPTO_CIPHER_AES_EAX;
		client->rsa.challenge_len = 16;
		rsa_aes_send_public_key(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY;
		break;
	case RFB_SECURITY_TYPE_RSA_AES256:
		client->rsa.hash_type = CRYPTO_HASH_SHA256;
		client->rsa.cipher_type = CRYPTO_CIPHER_AES256_EAX;
		client->rsa.challenge_len = 32;
		rsa_aes_send_public_key(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY;
		break;
#endif
	default:
		security_handshake_failed(client, NULL,
				"Unsupported security type");
		return -1;
	}

	return sizeof(type);
}

static void disconnect_all_other_clients(struct nvnc_client* client)
{
	struct nvnc_client* node;
	struct nvnc_client* tmp;

	LIST_FOREACH_SAFE (node, &client->server->clients, link, tmp)
		if (node != client) {
			nvnc_log(NVNC_LOG_DEBUG, "disconnect other client %p",
					node);
			nvnc_client_close(node);
		}

}

static int send_server_init_message(struct nvnc_client* client)
{
	struct nvnc* server = client->server;
	struct nvnc_display* display = server->display;

	size_t name_len = strlen(server->name);
	size_t size = sizeof(struct rfb_server_init_msg) + name_len;

	if (!display) {
		nvnc_log(NVNC_LOG_WARNING, "Tried to send init message, but no display has been added");
		goto close;
	}

	if (!display->buffer) {
		nvnc_log(NVNC_LOG_WARNING, "Tried to send init message, but no framebuffers have been set");
		goto close;
	}

	uint16_t width = nvnc_fb_get_width(display->buffer);
	uint16_t height = nvnc_fb_get_height(display->buffer);
	uint32_t fourcc = nvnc_fb_get_fourcc_format(display->buffer);

	if (rfb_pixfmt_from_fourcc(&client->pixfmt, fourcc) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to convert buffer format to RFB pixel format");
		goto close;
	}

	/* According to rfc6143, bpp must be 8, 16 or 32, but we can handle 24
	 * internally, so we just nudge 24 to 32 before reporting the pixel
	 * format to the client.
	 */
	if (client->pixfmt.bits_per_pixel == 24)
		client->pixfmt.bits_per_pixel = 32;

	struct rfb_server_init_msg* msg = calloc(1, size);
	if (!msg)
		goto close;

	msg->width = htons(width);
	msg->height = htons(height);
	msg->name_length = htonl(name_len);
	memcpy(msg->name_string, server->name, name_len);

	memcpy(&msg->pixel_format, &client->pixfmt, sizeof(msg->pixel_format));

	msg->pixel_format.red_max = htons(msg->pixel_format.red_max);
	msg->pixel_format.green_max = htons(msg->pixel_format.green_max);
	msg->pixel_format.blue_max = htons(msg->pixel_format.blue_max);

	struct rcbuf* payload = rcbuf_new(msg, size);
	stream_send(client->net_stream, payload, NULL, NULL);

	client->known_width = width;
	client->known_height = height;
	return 0;

close:
	nvnc_client_close(client);
	return -1;
}

static int on_init_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t shared_flag = client->msg_buffer[client->buffer_index];
	if (!shared_flag)
		disconnect_all_other_clients(client);

	update_min_rtt(client);

	if (send_server_init_message(client) == -1)
		return -1;

	nvnc_client_fn fn = client->server->new_client_fn;
	if (fn)
		fn(client);

	nvnc_log(NVNC_LOG_INFO, "Client %p initialised. MIN-RTT during handshake was %"PRId32" ms",
			client, client->min_rtt / 1000);

	client->state = VNC_CLIENT_STATE_READY;
	return sizeof(shared_flag);
}

static int cook_pixel_map(struct nvnc_client* client)
{
	struct rfb_pixel_format* fmt = &client->pixfmt;

	// We'll just pretend that this is rgb332
	fmt->true_colour_flag = true;
	fmt->big_endian_flag = false;
	fmt->bits_per_pixel = 8;
	fmt->depth = 8;
	fmt->red_max = 7;
	fmt->green_max = 7;
	fmt->blue_max = 3;
	fmt->red_shift = 5;
	fmt->green_shift = 2;
	fmt->blue_shift = 0;

	uint8_t buf[sizeof(struct rfb_set_colour_map_entries_msg)
		+ 256 * sizeof(struct rfb_colour_map_entry)];
	struct rfb_set_colour_map_entries_msg* msg =
		(struct rfb_set_colour_map_entries_msg*)buf;
	make_rgb332_pal8_map(msg);
	return stream_write(client->net_stream, buf, sizeof(buf), NULL, NULL);
}

static int on_client_set_pixel_format(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index <
	    4 + sizeof(struct rfb_pixel_format))
		return 0;

	struct rfb_pixel_format* fmt =
	        (struct rfb_pixel_format*)(client->msg_buffer +
	                                   client->buffer_index + 4);

	if (fmt->true_colour_flag) {
		nvnc_log(NVNC_LOG_DEBUG, "Using color palette for client %p",
				client);
		fmt->red_max = ntohs(fmt->red_max);
		fmt->green_max = ntohs(fmt->green_max);
		fmt->blue_max = ntohs(fmt->blue_max);
		memcpy(&client->pixfmt, fmt, sizeof(client->pixfmt));
	} else {
		nvnc_log(NVNC_LOG_DEBUG, "Using color palette for client %p",
				client);
		cook_pixel_map(client);
	}

	client->formats_changed = true;

	nvnc_log(NVNC_LOG_DEBUG, "Client %p chose pixel format: %s", client,
			rfb_pixfmt_to_string(&client->pixfmt));

	return 4 + sizeof(struct rfb_pixel_format);
}

static void encodings_to_string_list(char* dst, size_t len,
		enum rfb_encodings* encodings, size_t n)
{
	size_t off = 0;

	if (n > 0)
		off += snprintf(dst, len, "%s",
				encoding_to_string(encodings[0]));

	for (size_t i = 1; i < n && off < len; ++i)
		off += snprintf(dst + off, len - off, ",%s",
				encoding_to_string(encodings[i]));
}

static void nvnc_send_end_of_continuous_updates(struct nvnc_client* client)
{
	struct rfb_end_of_continuous_updates_msg msg;

	msg.type = RFB_SERVER_TO_CLIENT_END_OF_CONTINUOUS_UPDATES;

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
}

static void send_fence(struct nvnc_client* client, uint32_t flags,
		const void* payload, size_t length)
{
	assert(length <= 64);
	const uint8_t buffer[sizeof(struct rfb_fence_msg) + 64] = {};
	struct rfb_fence_msg *head = (struct rfb_fence_msg*)buffer;

	head->type = RFB_SERVER_TO_CLIENT_FENCE;
	head->flags = htonl(flags);
	head->length = length;
	memcpy(head->payload, payload, length);

	stream_write(client->net_stream, buffer, sizeof(*head) + length, NULL,
			NULL);
}

static void send_ping(struct nvnc_client* client, uint32_t prev_frame_size)
{
	if (!client_has_encoding(client, RFB_ENCODING_FENCE))
		return;

	uint32_t now = gettime_us(CLOCK_MONOTONIC);

	uint32_t payload[] = {
		htonl(now),
		htonl(prev_frame_size),
	};

	client->inflight_bytes += prev_frame_size;

	send_fence(client, RFB_FENCE_REQUEST | RFB_FENCE_BLOCK_BEFORE,
			payload, sizeof(payload));
}

static int on_client_set_encodings(struct nvnc_client* client)
{
	struct rfb_client_set_encodings_msg* msg =
	        (struct rfb_client_set_encodings_msg*)(client->msg_buffer +
	                                               client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	size_t n_encodings = ntohs(msg->n_encodings);
	size_t n = 0;

	if (client->buffer_len - client->buffer_index <
	    sizeof(*msg) + n_encodings * 4)
		return 0;

	client->quality = 10;

	for (size_t i = 0; i < n_encodings && n < MAX_ENCODINGS; ++i) {
		enum rfb_encodings encoding = htonl(msg->encodings[i]);

		switch (encoding) {
		case RFB_ENCODING_RAW:
		case RFB_ENCODING_COPYRECT:
		case RFB_ENCODING_RRE:
		case RFB_ENCODING_HEXTILE:
		case RFB_ENCODING_TIGHT:
		case RFB_ENCODING_TRLE:
		case RFB_ENCODING_ZRLE:
		case RFB_ENCODING_OPEN_H264:
		case RFB_ENCODING_CURSOR:
		case RFB_ENCODING_DESKTOPSIZE:
		case RFB_ENCODING_EXTENDEDDESKTOPSIZE:
		case RFB_ENCODING_QEMU_EXT_KEY_EVENT:
		case RFB_ENCODING_QEMU_LED_STATE:
		case RFB_ENCODING_VMWARE_LED_STATE:
		case RFB_ENCODING_EXTENDED_CLIPBOARD:
		case RFB_ENCODING_CONTINUOUSUPDATES:
		case RFB_ENCODING_EXT_MOUSE_BUTTONS:
		case RFB_ENCODING_FENCE:
#ifdef ENABLE_EXPERIMENTAL
		case RFB_ENCODING_PTS:
		case RFB_ENCODING_NTP:
#endif
			client->encodings[n++] = encoding;
#ifndef ENABLE_EXPERIMENTAL
		case RFB_ENCODING_PTS:
		case RFB_ENCODING_NTP:
			;
#endif
			break;
		}

		if (RFB_ENCODING_JPEG_LOWQ <= encoding &&
				encoding <= RFB_ENCODING_JPEG_HIGHQ)
			client->quality = encoding - RFB_ENCODING_JPEG_LOWQ;
	}

	char encoding_list[256] = {};
	encodings_to_string_list(encoding_list, sizeof(encoding_list),
			client->encodings, n);
	nvnc_log(NVNC_LOG_DEBUG, "Client %p set encodings: %s", client,
			encoding_list);

	client->n_encodings = n;
	client->formats_changed = true;

	if (!client->is_continuous_updates_notified &&
			client_has_encoding(client, RFB_ENCODING_CONTINUOUSUPDATES)) {
		nvnc_send_end_of_continuous_updates(client);
		client->is_continuous_updates_notified = true;
	}

	if (client_has_encoding(client, RFB_ENCODING_EXTENDED_CLIPBOARD))
		send_ext_clipboard_caps(client);

	if (client_has_encoding(client, RFB_ENCODING_FENCE))
		send_ping(client, 0);

	return sizeof(*msg) + 4 * n_encodings;
}

static void send_cursor_update(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct vec payload;
	vec_init(&payload, 4096);

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};

	vec_append(&payload, &head, sizeof(head));

	int rc = cursor_encode(&payload, &client->pixfmt, server->cursor.buffer,
			server->cursor.width, server->cursor.height,
			server->cursor.hotspot_x, server->cursor.hotspot_y);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to send cursor to client");
		vec_destroy(&payload);
		return;
	}

	client->cursor_seq = server->cursor_seq;

	stream_send(client->net_stream, rcbuf_new(payload.data, payload.len),
			NULL, NULL);
}

static bool will_send_pts(const struct nvnc_client* client, uint64_t pts)
{
	return pts != NVNC_NO_PTS && client_has_encoding(client, RFB_ENCODING_PTS);
}

static int send_pts_rect(struct nvnc_client* client, uint64_t pts)
{
	if (!will_send_pts(client, pts))
		return 0;

	uint8_t buf[sizeof(struct rfb_server_fb_rect) + 8] = { 0 };
	struct rfb_server_fb_rect* head = (struct rfb_server_fb_rect*)buf;
	head->encoding = htonl(RFB_ENCODING_PTS);
	uint64_t* msg_pts = (uint64_t*)&buf[sizeof(struct rfb_server_fb_rect)];
	*msg_pts = nvnc__htonll(pts);

	return stream_write(client->net_stream, buf, sizeof(buf), NULL, NULL);
}

static const char* encoding_to_string(enum rfb_encodings encoding)
{
	switch (encoding) {
	case RFB_ENCODING_RAW: return "raw";
	case RFB_ENCODING_COPYRECT: return "copyrect";
	case RFB_ENCODING_RRE: return "rre";
	case RFB_ENCODING_HEXTILE: return "hextile";
	case RFB_ENCODING_TIGHT: return "tight";
	case RFB_ENCODING_TRLE: return "trle";
	case RFB_ENCODING_ZRLE: return "zrle";
	case RFB_ENCODING_OPEN_H264: return "open-h264";
	case RFB_ENCODING_CURSOR: return "cursor";
	case RFB_ENCODING_DESKTOPSIZE: return "desktop-size";
	case RFB_ENCODING_EXTENDEDDESKTOPSIZE: return "extended-desktop-size";
	case RFB_ENCODING_QEMU_EXT_KEY_EVENT: return "qemu-extended-key-event";
	case RFB_ENCODING_QEMU_LED_STATE: return "qemu-led-state";
	case RFB_ENCODING_VMWARE_LED_STATE: return "vmware-led-state";
	case RFB_ENCODING_EXTENDED_CLIPBOARD: return "extended-clipboard";
	case RFB_ENCODING_PTS: return "pts";
	case RFB_ENCODING_NTP: return "ntp";
	case RFB_ENCODING_CONTINUOUSUPDATES: return "continuous-updates";
	case RFB_ENCODING_FENCE: return "fence";
	case RFB_ENCODING_EXT_MOUSE_BUTTONS: return "extended-mouse-buttons";
	}
	return "UNKNOWN";
}

static bool ensure_encoder(struct nvnc_client* client, const struct nvnc_fb *fb)
{
	struct nvnc* server = client->server;

	enum rfb_encodings encoding = choose_frame_encoding(client, fb);
	if (client->encoder && encoding == encoder_get_type(client->encoder))
		return true;

	int width = server->display->buffer->width;
	int height = server->display->buffer->height;
	if (client->encoder) {
		server->n_damage_clients -= !(client->encoder->impl->flags &
				ENCODER_IMPL_FLAG_IGNORES_DAMAGE);
		client->encoder->on_done = NULL;
		client->encoder->userdata = NULL;
	}
	encoder_unref(client->encoder);

	/* Zlib streams need to be saved so we keep encoders around that
	 * use them.
	 */
	switch (encoding) {
	case RFB_ENCODING_ZRLE:
		if (!client->zrle_encoder) {
			client->zrle_encoder =
				encoder_new(encoding, width, height);
		}
		client->encoder = client->zrle_encoder;
		encoder_ref(client->encoder);
		break;
	case RFB_ENCODING_TIGHT:
		if (!client->tight_encoder) {
			client->tight_encoder =
				encoder_new(encoding, width, height);
		}
		client->encoder = client->tight_encoder;
		encoder_ref(client->encoder);
		break;
	default:
		client->encoder = encoder_new(encoding, width, height);
		break;
	}

	if (!client->encoder) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to allocate new encoder");
		return false;
	}

	server->n_damage_clients += !(client->encoder->impl->flags &
			ENCODER_IMPL_FLAG_IGNORES_DAMAGE);

	nvnc_log(NVNC_LOG_INFO, "Choosing %s encoding for client %p",
			encoding_to_string(encoding), client);

	return true;
}

static int decrement_pending_requests(struct nvnc_client* client)
{
	assert(!client->is_updating);
	if (client->continuous_updates_enabled)
		return 1;
	process_pending_fence(client);
	return --client->n_pending_requests;
}

/* TODO: This should be const but older versions of pixman do not use const for
 * regions. This has been fixed, but Ubuntu is slow on the uptake as usual,
 * so this will remain non-const for now.
 */
static bool client_has_damage(struct nvnc_client* client)
{
	if (!pixman_region_not_empty(&client->damage))
		return false;

	/* Skip continuous updates without damage inside specified rectangle */
	if (!client->continuous_updates_enabled)
		return true;

	struct pixman_region16 damage;
	pixman_region_init(&damage);
	pixman_region_intersect_rect(&damage, &client->damage,
			client->continuous_updates.x,
			client->continuous_updates.y,
			client->continuous_updates.width,
			client->continuous_updates.height);
	bool result = pixman_region_not_empty(&damage);
	pixman_region_fini(&damage);

	return result;
}

static void process_fb_update_requests(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (client->net_stream->state == STREAM_STATE_CLOSED)
		return;

	if (!server->display || !server->display->buffer)
		return;

	if (client->is_updating)
		return;

	if (!client->continuous_updates_enabled &&
	    client->n_pending_requests == 0)
		return;

	struct nvnc_fb* fb = client->server->display->buffer;
	assert(fb);

	if (!client->is_ext_notified) {
		client->is_ext_notified = true;

		if (send_ext_support_frame(client)) {
			if (decrement_pending_requests(client) <= 0)
				return;
		}
	}

	if (server->cursor_seq != client->cursor_seq
			&& client_has_encoding(client, RFB_ENCODING_CURSOR)) {
		send_cursor_update(client);

		if (decrement_pending_requests(client) <= 0)
			return;
	}

	if (client_send_led_state(client)) {
		if (decrement_pending_requests(client) <= 0)
			return;
	}

	if (!client_has_damage(client))
		return;

	int bandwidth = bwe_get_estimate(client->bwe);
	if (bandwidth != 0) {
		double max_delay = 33.333e-3;
		int max_inflight = round(max_delay + 1e-6 *
				client->min_rtt * bandwidth);

		// If there is already more data inflight than the link can
		// handle, let's not put more load on it:
		if (client->inflight_bytes > max_inflight) {
			nvnc_log(NVNC_LOG_DEBUG, "Exceeded bandwidth limit. Dropping frame.");
			return;
		}
	}

	if (!ensure_encoder(client, fb))
		return;

	DTRACE_PROBE1(neatvnc, update_fb_start, client);

	/* The client's damage is exchanged for an empty one */
	struct pixman_region16 damage = client->damage;
	pixman_region_init(&client->damage);

	client->is_updating = true;
	client->formats_changed = false;

	encoder_set_quality(client->encoder, client->quality);
	encoder_set_output_format(client->encoder, &client->pixfmt);

	client->encoder->on_done = on_encode_frame_done;
	client->encoder->userdata = client;

	DTRACE_PROBE2(neatvnc, process_fb_update_requests__encode,
			client, fb->pts);

	/* Damage is clamped here in case the client requested an out of bounds
	 * region.
	 */
	pixman_region_intersect_rect(&damage, &damage, 0, 0, fb->width,
			fb->height);

	if (encoder_encode(client->encoder, fb, &damage) >= 0) {
		if (client->n_pending_requests > 0)
			--client->n_pending_requests;
	} else {
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode current frame");
		client->is_updating = false;
		client->formats_changed = false;
	}

	pixman_region_fini(&damage);
}

static int on_client_fb_update_request(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_client_fb_update_req_msg* msg =
	        (struct rfb_client_fb_update_req_msg*)(client->msg_buffer +
	                                               client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	int incremental = msg->incremental;
	int x = ntohs(msg->x);
	int y = ntohs(msg->y);
	int width = ntohs(msg->width);
	int height = ntohs(msg->height);

	if (incremental && client->continuous_updates_enabled)
		return 0;

	client->n_pending_requests++;

	/* Note: The region sent from the client is ignored for incremental
	 * updates. This avoids superfluous complexity.
	 */
	if (!incremental) {
		pixman_region_union_rect(&client->damage, &client->damage, x, y,
				width, height);

		if (client->encoder)
			encoder_request_key_frame(client->encoder);
	}

	DTRACE_PROBE1(neatvnc, update_fb_request, client);

	nvnc_fb_req_fn fn = server->fb_req_fn;
	if (fn)
		fn(client, incremental, x, y, width, height);

	if (!incremental &&
	    client_has_encoding(client, RFB_ENCODING_EXTENDEDDESKTOPSIZE)) {
		client->known_width = 0;
		client->known_height = 0;
	}

	process_fb_update_requests(client);

	return sizeof(*msg);
}

static int on_client_key_event(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_client_key_event_msg* msg =
	        (struct rfb_client_key_event_msg*)(client->msg_buffer +
	                                           client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	int down_flag = msg->down_flag;
	uint32_t keysym = ntohl(msg->key);

	nvnc_key_fn fn = server->key_fn;
	if (fn)
		fn(client, keysym, !!down_flag);

	return sizeof(*msg);
}

static int on_client_qemu_key_event(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_client_qemu_key_event_msg* msg =
	        (struct rfb_client_qemu_key_event_msg*)(client->msg_buffer +
		                                        client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	int down_flag = msg->down_flag;
	uint32_t xt_keycode = ntohl(msg->keycode);
	uint32_t keysym = ntohl(msg->keysym);


	uint32_t evdev_keycode = 0;
	if (xt_keycode < code_map_qnum_to_linux_len) {
		evdev_keycode = code_map_qnum_to_linux[xt_keycode];
	} else {
		nvnc_log(NVNC_LOG_WARNING, "Received too large key code from client: %" PRIu32,
				xt_keycode);
	}
	if (!evdev_keycode)
		evdev_keycode = xt_keycode;

	nvnc_key_fn key_code_fn = server->key_code_fn;
	nvnc_key_fn key_fn = server->key_fn;

	if (key_code_fn)
		key_code_fn(client, evdev_keycode, !!down_flag);
	else if (key_fn)
		key_fn(client, keysym, !!down_flag);

	return sizeof(*msg);
}

static int on_client_qemu_event(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 2)
		return 0;

	enum rfb_client_to_server_qemu_msg_type subtype =
	        client->msg_buffer[client->buffer_index + 1];

	switch (subtype) {
	case RFB_CLIENT_TO_SERVER_QEMU_KEY_EVENT:
		return on_client_qemu_key_event(client);
	}

	nvnc_log(NVNC_LOG_WARNING, "Got uninterpretable qemu message from client: %p",
			client);
	nvnc_client_close(client);
	return -1;
}

static int on_client_pointer_event(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_client_pointer_event_msg* msg =
	        (struct rfb_client_pointer_event_msg*)(client->msg_buffer +
	                                               client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	int button_mask = msg->button_mask;
	int message_size = sizeof(*msg);

	if (client->has_ext_mouse_buttons && (button_mask & 0x80)) {
		struct rfb_ext_client_pointer_event_msg* ext_msg =
			(struct rfb_ext_client_pointer_event_msg*)msg;

		if (client->buffer_len - client->buffer_index < sizeof(*ext_msg))
			return 0;

		button_mask &= 0x7f;
		button_mask |= ext_msg->ext_button_mask << 7;
		message_size = sizeof(*ext_msg);
	}

	uint16_t x = ntohs(msg->x);
	uint16_t y = ntohs(msg->y);

	nvnc_pointer_fn fn = server->pointer_fn;
	if (fn)
		fn(client, x, y, button_mask);

	return message_size;
}

static void send_ext_clipboard_caps(struct nvnc_client* client)
{
	struct rfb_ext_clipboard_msg msg = {};

	msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
	msg.length = htonl(-8);
	msg.flags = htonl(RFB_EXT_CLIPBOARD_CAPS |
			RFB_EXT_CLIPBOARD_FORMAT_TEXT |
			RFB_EXT_CLIPBOARD_ACTION_ALL);

	/* discourage unsolicited provide messages, instead force a
	 * client notify -> server request -> solicited client provide */
	uint32_t max_unsolicited_text_size = 0;

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
	stream_write(client->net_stream, &max_unsolicited_text_size,
			sizeof(max_unsolicited_text_size), NULL, NULL);
}

static void send_ext_clipboard_request(struct nvnc_client* client)
{
	struct rfb_ext_clipboard_msg msg = {};

	msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
	msg.length = htonl(-4);
	msg.flags = htonl(RFB_EXT_CLIPBOARD_ACTION_REQUEST |
			RFB_EXT_CLIPBOARD_FORMAT_TEXT);

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
}

static void send_ext_clipboard_notify(struct nvnc_client* client)
{
	struct rfb_ext_clipboard_msg msg = {};

	msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
	msg.length = htonl(-4);
	uint32_t flags = RFB_EXT_CLIPBOARD_ACTION_NOTIFY;
	if (client->server->ext_clipboard_provide_msg.buffer)
		flags |= RFB_EXT_CLIPBOARD_FORMAT_TEXT;
	msg.flags = htonl(flags);

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
}

static void send_ext_clipboard_provide(struct nvnc_client* client)
{
	assert(client->server->ext_clipboard_provide_msg.buffer);

	struct rfb_ext_clipboard_msg msg = {};

	msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
	msg.length = htonl(-(4 + client->server->ext_clipboard_provide_msg.length));
	msg.flags = htonl(RFB_EXT_CLIPBOARD_ACTION_PROVIDE |
			RFB_EXT_CLIPBOARD_FORMAT_TEXT);

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
	stream_write(client->net_stream,
			client->server->ext_clipboard_provide_msg.buffer,
			client->server->ext_clipboard_provide_msg.length,
			NULL, NULL);
}

static char* crlf_to_lf(const char* src, size_t len)
{
	/* caller will read this as a null-terminated string */
	char* lf_buf = malloc(len + 1);
	if (!lf_buf)
		return NULL;

	const char* in = src;
	size_t in_len = len;
	char* out = lf_buf;
	while (in_len > 0) {
		if (*in != '\r') {
			*out++ = *in++;
			in_len--;
			continue;
		}

		if ((in_len == 1) || (*(in + 1) != '\n'))
			*out++ = '\n';

		in++;
		in_len--;
	}
	*out = 0;

	return lf_buf;
}

static void process_client_ext_clipboard_provide(struct nvnc_client* client,
		unsigned char* zlib_data, size_t zlib_len)
{
	int rc;

	z_stream zs;
	zs.zalloc = NULL;
	zs.zfree = NULL;
	zs.opaque = NULL;
	zs.avail_in = 0;
	zs.next_in = NULL;
	if (inflateInit(&zs) != Z_OK)
		return;

	uint32_t inflate_len;

	zs.avail_in = zlib_len;
	zs.next_in = zlib_data;
	zs.avail_out = 4;
	zs.next_out = (unsigned char*)&inflate_len;
	rc = inflate(&zs, Z_SYNC_FLUSH);
	if (rc != Z_OK) {
		nvnc_log(NVNC_LOG_WARNING, "Failed to inflate client's clipboard text: %p",
				client);
		inflateEnd(&zs);
		return;
	}
	inflate_len = ntohl(inflate_len);

	if (inflate_len <= 1) {
		nvnc_log(NVNC_LOG_DEBUG, "Client sent empty clipboard update: %p",
				client);
		inflateEnd(&zs);
		return;
	}

	unsigned char* inflate_buf = malloc(inflate_len);
	if (!inflate_buf) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		inflateEnd(&zs);
		return;
	}

	zs.avail_out = inflate_len;
	zs.next_out = inflate_buf;
	rc = inflate(&zs, Z_SYNC_FLUSH);
	inflateEnd(&zs);
	if (rc != Z_OK && rc != Z_STREAM_END) {
		nvnc_log(NVNC_LOG_WARNING, "Failed to inflate client's clipboard text: %p",
				client);
		free(inflate_buf);
		return;
	}

	if (inflate_buf[inflate_len - 1]) {
		nvnc_log(NVNC_LOG_WARNING, "Client sent badly formatted clipboard text: %p",
				client);
		free(inflate_buf);
		return;
	}

	char* converted_buf = crlf_to_lf((const char*)inflate_buf, inflate_len - 1);
	free(inflate_buf);
	if (!converted_buf) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		return;
	}
	size_t converted_len = strlen(converted_buf);

	nvnc_cut_text_fn fn = client->server->cut_text_fn;
	if (fn)
		fn(client, converted_buf, converted_len);

	free(converted_buf);
}

static int process_client_ext_clipboard(struct nvnc_client* client)
{
	struct rfb_ext_clipboard_msg* msg =
		(struct rfb_ext_clipboard_msg*)(client->msg_buffer +
					client->buffer_index);

	size_t left_to_process = client->buffer_len - client->buffer_index;

	if (left_to_process < sizeof(*msg))
		return 0;

	int32_t length = -(ntohl(msg->length));
	length = length - 4; /* length starting from after flags */

	uint32_t flags = ntohl(msg->flags);

	/* make sure that there is space to read a correctly-sized caps message
	 * right now */
	if (flags & RFB_EXT_CLIPBOARD_CAPS)
		if (left_to_process < sizeof(*msg) + MIN(16 * 4, length))
			return 0;

	int32_t max_length = MAX_CUT_TEXT_SIZE;

	/* Messages greater than this size are unsupported */
	if (length > max_length) {
		nvnc_log(NVNC_LOG_ERROR, "Extended clipboard payload length (%d) is greater than max supported length (%d)",
				length, max_length);
		nvnc_client_close(client);
		return -1;
	}

	size_t msg_size = sizeof(*msg) + length;

	/* this is expected to be a text provide message. if not, tell
	 * process_big_cut_text to ignore it, to avoid unnecessarily attempting
	 * to inflate garbage */
	if (msg_size > left_to_process) {
		assert(!client->cut_text.buffer);
		client->cut_text.buffer = malloc(length);
		if (!client->cut_text.buffer) {
			nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
			nvnc_client_close(client);
			return -1;
		}

		size_t partial_size = left_to_process - sizeof(*msg);

		memcpy(client->cut_text.buffer, msg->zlib_stream, partial_size);

		client->cut_text.is_zlib = true;
		client->cut_text.length = length;
		client->cut_text.index = partial_size;

		client->cut_text.is_text_provide = (flags & RFB_EXT_CLIPBOARD_ACTION_PROVIDE &&
				flags & RFB_EXT_CLIPBOARD_FORMAT_TEXT &&
				!(flags & RFB_EXT_CLIPBOARD_CAPS));

		return left_to_process;
	}

	if (flags & RFB_EXT_CLIPBOARD_CAPS) {
		client->ext_clipboard_caps = flags;

		/* we only care about text, which will always be
		 * listed first */
		if (length >= 4)
			client->ext_clipboard_max_unsolicited_text_size =
				ntohl(msg->max_unsolicited_sizes[0]);
	} else if ((flags & RFB_EXT_CLIPBOARD_ACTION_REQUEST) &&
			(flags & RFB_EXT_CLIPBOARD_FORMAT_TEXT) &&
			(client->ext_clipboard_caps & RFB_EXT_CLIPBOARD_ACTION_PROVIDE) &&
			(client->server->ext_clipboard_provide_msg.buffer)) {
		send_ext_clipboard_provide(client);
	} else if ((flags & RFB_EXT_CLIPBOARD_ACTION_PEEK) &&
			(client->ext_clipboard_caps & RFB_EXT_CLIPBOARD_ACTION_NOTIFY)) {
		send_ext_clipboard_notify(client);
	} else if ((flags & RFB_EXT_CLIPBOARD_ACTION_NOTIFY) &&
			(flags & RFB_EXT_CLIPBOARD_FORMAT_TEXT) &&
			(client->ext_clipboard_caps & RFB_EXT_CLIPBOARD_ACTION_REQUEST)) {
		send_ext_clipboard_request(client);
	} else if ((flags & RFB_EXT_CLIPBOARD_ACTION_PROVIDE) &&
			(flags & RFB_EXT_CLIPBOARD_FORMAT_TEXT)) {
		process_client_ext_clipboard_provide(client, msg->zlib_stream, length);
	}

	return msg_size;
}

static int process_client_cut_text(struct nvnc_client* client)
{
	struct rfb_cut_text_msg* msg =
		(struct rfb_cut_text_msg*)(client->msg_buffer +
					   client->buffer_index);

	size_t left_to_process = client->buffer_len - client->buffer_index;

	uint32_t length = ntohl(msg->length);
	uint32_t max_length = MAX_CUT_TEXT_SIZE;

	/* Messages greater than this size are unsupported */
	if (length > max_length) {
		nvnc_log(NVNC_LOG_ERROR, "Copied text length (%d) is greater than max supported length (%d)",
				length, max_length);
		nvnc_client_close(client);
		return -1;
	}

	size_t msg_size = sizeof(*msg) + length;

	if (msg_size <= left_to_process) {
		nvnc_cut_text_fn fn = client->server->cut_text_fn;
		if (fn)
			fn(client, msg->text, length);

		return msg_size;
	}

	assert(!client->cut_text.buffer);

	client->cut_text.buffer = malloc(length);
	if (!client->cut_text.buffer) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		nvnc_client_close(client);
		return -1;
	}

	size_t partial_size = left_to_process - sizeof(*msg);

	memcpy(client->cut_text.buffer, msg->text, partial_size);

	client->cut_text.is_zlib = false;
	client->cut_text.is_text_provide = false;
	client->cut_text.length = length;
	client->cut_text.index = partial_size;

	return left_to_process;
}

static int on_client_cut_text(struct nvnc_client* client)
{
	struct rfb_cut_text_msg* msg =
		(struct rfb_cut_text_msg*)(client->msg_buffer +
					   client->buffer_index);

	size_t left_to_process = client->buffer_len - client->buffer_index;

	if (left_to_process < sizeof(*msg))
		return 0;

	if (client_has_encoding(client, RFB_ENCODING_EXTENDED_CLIPBOARD) &&
			((int32_t)ntohl(msg->length) < 0))
		return process_client_ext_clipboard(client);

	return process_client_cut_text(client);
}

static int on_client_enable_continuous_updates(struct nvnc_client* client)
{
	struct rfb_enable_continuous_updates_msg* msg =
	        (struct rfb_enable_continuous_updates_msg*)
	        (client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	client->continuous_updates_enabled = msg->enable_flag;

	if (msg->enable_flag) {
		client->continuous_updates.x = ntohs(msg->x);
		client->continuous_updates.y = ntohs(msg->y);
		client->continuous_updates.width = ntohs(msg->width);
		client->continuous_updates.height = ntohs(msg->height);

		/* If there are any pending messages left, make sure they are processed */
		process_fb_update_requests(client);
	} else {
		client->continuous_updates.x = 0;
		client->continuous_updates.y = 0;
		client->continuous_updates.width = 0;
		client->continuous_updates.height = 0;

		nvnc_send_end_of_continuous_updates(client);
	}

	return sizeof(*msg);
}

static void process_big_cut_text(struct nvnc_client* client)
{
	assert(client->cut_text.length > client->cut_text.index);

	void* start = client->cut_text.buffer + client->cut_text.index;
	size_t space = client->cut_text.length - client->cut_text.index;

	space = MIN(space, MSG_BUFFER_SIZE);

	ssize_t n_read = stream_read(client->net_stream, start, space);

	if (n_read == 0)
		return;

	if (n_read < 0) {
		if (errno != EAGAIN) {
			nvnc_log(NVNC_LOG_INFO, "Client connection error: %p",
					client);
			nvnc_client_close(client);
		}

		return;
	}

	client->cut_text.index += n_read;

	if (client->cut_text.index != client->cut_text.length)
		return;

	if (client->cut_text.is_zlib) {
		if (client->cut_text.is_text_provide)
			process_client_ext_clipboard_provide(client,
					(unsigned char*)client->cut_text.buffer,
					client->cut_text.length);
	} else {
		nvnc_cut_text_fn fn = client->server->cut_text_fn;
		if (fn)
			fn(client, client->cut_text.buffer,
					client->cut_text.length);
	}

	free(client->cut_text.buffer);
	client->cut_text.buffer = NULL;
}

static void ext_clipboard_save_provide_msg(struct nvnc* server, const char* text,
		uint32_t len)
{
	if (server->ext_clipboard_provide_msg.buffer || !len) {
		free(server->ext_clipboard_provide_msg.buffer);
		server->ext_clipboard_provide_msg.buffer = NULL;
	}

	if (!len)
		return;

	/* requires null terminator */
	size_t provide_msg_len = 4 + len + 1;
	unsigned char* provide_msg_buf = malloc(provide_msg_len);
	if (!provide_msg_buf) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		return;
	}

	uint32_t nlen = htonl(len);
	memcpy(provide_msg_buf, &nlen, 4);
	memcpy(provide_msg_buf + 4, text, len);
	provide_msg_buf[provide_msg_len - 1] = 0;

	unsigned long length = compressBound(provide_msg_len);
	server->ext_clipboard_provide_msg.buffer = malloc(length);
	if (!server->ext_clipboard_provide_msg.buffer) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		free(provide_msg_buf);
		return;
	}

	int rc;
	rc = compress((unsigned char*)server->ext_clipboard_provide_msg.buffer,
			&length, provide_msg_buf, provide_msg_len);
	server->ext_clipboard_provide_msg.length = length;

	free(provide_msg_buf);

	if (rc != Z_OK) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to compress extended clipboard payload");
		free(server->ext_clipboard_provide_msg.buffer);
		server->ext_clipboard_provide_msg.buffer = NULL;
	}
}

static void send_cut_text_to_client(struct nvnc_client* client,
		const char* text, uint32_t len)
{
       struct rfb_cut_text_msg msg = {};

       msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
       msg.length = htonl(len);

       stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
       stream_write(client->net_stream, text, len, NULL, NULL);
}

EXPORT
void nvnc_send_cut_text(struct nvnc* server, const char* text, uint32_t len)
{
	struct nvnc_client* client;

	bool ext_clipboard_in_use = false;
	LIST_FOREACH (client, &server->clients, link) {
		if (client_has_encoding(client, RFB_ENCODING_EXTENDED_CLIPBOARD)) {
			ext_clipboard_in_use = true;
			break;
		}
	}

	if (ext_clipboard_in_use) {
		ext_clipboard_save_provide_msg(server, text, len);
	} else if (server->ext_clipboard_provide_msg.buffer) {
		free(server->ext_clipboard_provide_msg.buffer);
		server->ext_clipboard_provide_msg.buffer = NULL;
	}

	LIST_FOREACH (client, &server->clients, link) {
		if (client_has_encoding(client, RFB_ENCODING_EXTENDED_CLIPBOARD)) {
			if (!server->ext_clipboard_provide_msg.buffer)
				continue;

			if (client->ext_clipboard_caps & RFB_EXT_CLIPBOARD_ACTION_PROVIDE &&
					len <= client->ext_clipboard_max_unsolicited_text_size)
				send_ext_clipboard_provide(client);
			else if (client->ext_clipboard_caps & RFB_EXT_CLIPBOARD_ACTION_NOTIFY)
				send_ext_clipboard_notify(client);
		} else {
			send_cut_text_to_client(client, text, len);
		}
	}
}

static enum rfb_resize_status check_desktop_layout(struct nvnc_client* client,
		uint16_t width, uint16_t height, uint8_t n_screens,
		struct rfb_screen* screens)
{
	struct nvnc* server = client->server;
	struct nvnc_desktop_layout* layout;
	enum rfb_resize_status status = RFB_RESIZE_STATUS_REQUEST_FORWARDED;

	layout = malloc(sizeof(*layout) +
			n_screens * sizeof(*layout->display_layouts));
	if (!layout)
		return RFB_RESIZE_STATUS_OUT_OF_RESOURCES;

	layout->width = width;
	layout->height = height;
	layout->n_display_layouts = n_screens;

	for (size_t i = 0; i < n_screens; ++i) {
		struct nvnc_display_layout* display;
		struct rfb_screen* screen;

		display = &layout->display_layouts[i];
		screen = &screens[i];

		nvnc_display_layout_init(display, screen);

		if (screen->id == 0)
			display->display = server->display;

		if (display->x_pos + display->width > width ||
		    display->y_pos + display->height > height) {
			status = RFB_RESIZE_STATUS_INVALID_LAYOUT;
			goto out;
		}
	}

	if (!server->desktop_layout_fn ||
	    !server->desktop_layout_fn(client, layout))
		status = RFB_RESIZE_STATUS_PROHIBITED;
out:
	free(layout);
	return status;
}

static void send_extended_desktop_size_rect(struct nvnc_client* client,
		uint16_t width, uint16_t height,
		enum rfb_resize_initiator initiator,
		enum rfb_resize_status status)
{
	nvnc_log(NVNC_LOG_DEBUG, "Sending extended desktop resize rect: %"PRIu16"x%"PRIu16,
			width, height);

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_EXTENDEDDESKTOPSIZE),
		.x = htons(initiator),
		.y = htons(status),
		.width = htons(width),
		.height = htons(height),
	};

	uint8_t number_of_screens = 1;
	uint8_t buf[4] = { number_of_screens };

	struct rfb_screen screen = {
		.width = htons(width),
		.height = htons(height),
	};

	stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	stream_write(client->net_stream, &buf, sizeof(buf), NULL, NULL);
	stream_write(client->net_stream, &screen, sizeof(screen), NULL, NULL);
}

static int on_client_set_desktop_size_event(struct nvnc_client* client)
{
	struct rfb_client_set_desktop_size_event_msg* msg =
		(struct rfb_client_set_desktop_size_event_msg*)
		(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint8_t n_screens = msg->number_of_screens;

	if (client->buffer_len - client->buffer_index < sizeof(*msg) +
			n_screens * sizeof(struct rfb_screen))
		return 0;

	uint16_t width = ntohs(msg->width);
	uint16_t height = ntohs(msg->height);

	enum rfb_resize_status status = check_desktop_layout(client, width,
			height, n_screens, msg->screens);

	nvnc_log(NVNC_LOG_DEBUG, "Client requested resize to %"PRIu16"x%"PRIu16", result: %d",
			width, height, status);

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};
	stream_write(client->net_stream, &head, sizeof(head), NULL, NULL);

	send_extended_desktop_size_rect(client, width, height,
			RFB_RESIZE_INITIATOR_THIS_CLIENT,
			status);

	return sizeof(*msg) + n_screens * sizeof(struct rfb_screen);
}

static void update_ntp_stats(struct nvnc_client* client,
		const struct rfb_ntp_msg *msg)
{
	uint32_t t0 = ntohl(msg->t0);
	uint32_t t1 = ntohl(msg->t1);
	uint32_t t2 = ntohl(msg->t2);
	uint32_t t3 = ntohl(msg->t3);

	double delta = (int32_t)(t3 - t0) - (int32_t)(t2 - t1);
	double theta = ((int32_t)(t1 - t0) + (int32_t)(t2 - t3)) / 2;

	nvnc_log(NVNC_LOG_DEBUG, "NTP: delta: %.2f ms, theta: %.2f ms",
			delta / 1e3, theta / 1e3);
}

static struct rcbuf* on_ntp_msg_send(struct stream* tcp_stream,
		void* userdata)
{
	struct rfb_ntp_msg* msg = userdata;
	msg->t2 = htonl(gettime_us(CLOCK_MONOTONIC));
	return rcbuf_from_mem(msg, sizeof(*msg));
}

static int on_client_ntp(struct nvnc_client* client)
{
	struct rfb_ntp_msg msg;

	if (client->buffer_len - client->buffer_index < sizeof(msg))
		return 0;

	memcpy(&msg, client->msg_buffer + client->buffer_index, sizeof(msg));

	if (msg.t3 != 0) {
		update_ntp_stats(client, &msg);
		return sizeof(msg);
	}

	msg.t1 = htonl(gettime_us(CLOCK_MONOTONIC));

	struct rfb_ntp_msg* out_msg = malloc(sizeof(*out_msg));
	assert(out_msg);
	memcpy(out_msg, &msg, sizeof(*out_msg));

	// The callback gets executed as the message is leaving the send queue
	// so that we can set t2 as late as possible.
	stream_exec_and_send(client->net_stream, on_ntp_msg_send, out_msg);

	return sizeof(msg);
}

static bool on_fence_request(struct nvnc_client* client,
		enum rfb_fence_flags flags, const void* payload, size_t length)
{
	flags &= RFB_FENCE_MASK;

	// If a fence is already pending, we can't process this fence request.
	// This is what we'll want to do anyway if BLOCK_AFTER is set.
	// TODO: Queue pending fences?
	if (client->n_pending_requests > 0) {
		client->is_blocked_by_fence = true;
		return false;
	}

	if ((flags & RFB_FENCE_BLOCK_BEFORE) &&
			client->n_pending_requests + client->is_updating > 0) {
		client->pending_fence.n_pending_requests =
			client->n_pending_requests + client->is_updating;
	} else if ((flags & RFB_FENCE_SYNC_NEXT) && client->is_updating) {
		client->pending_fence.n_pending_requests = 1;
		client->must_block_after_next_message =
			!!(flags & RFB_FENCE_BLOCK_AFTER);
	}

	if (client->pending_fence.n_pending_requests == 0) {
		send_fence(client, flags, payload, length);
	} else {
		client->is_blocked_by_fence =
			flags == (RFB_FENCE_BLOCK_BEFORE | RFB_FENCE_BLOCK_AFTER);
		client->pending_fence.flags = flags;
		client->pending_fence.length = length;
		memcpy(client->pending_fence.payload, payload, length);
	}

	return true;
}

static void on_fence_response(struct nvnc_client* client,
		enum rfb_fence_flags flags, const void* payload, size_t length)
{
	// We're only using this for pings

	const uint32_t *payload_u32 = payload;

	uint32_t departure_time_be;
	uint32_t frame_size_be;

	memcpy(&departure_time_be, &payload_u32[0], sizeof(departure_time_be));
	memcpy(&frame_size_be, &payload_u32[1], sizeof(frame_size_be));

	int32_t departure_time = ntohl(departure_time_be);
	uint32_t frame_size = ntohl(frame_size_be);

	if (frame_size == 0)
		return;

	int32_t now = gettime_us(CLOCK_MONOTONIC);
	int32_t rtt = now - departure_time;
	if (rtt < 0) {
		nvnc_log(NVNC_LOG_WARNING, "Got negative RTT on ping response");
		return;
	}

	if (rtt < client->min_rtt) {
		client->min_rtt = rtt;
		bwe_update_rtt_min(client->bwe, rtt);
	}

	struct bwe_sample sample = {
		.bytes = frame_size + sizeof(struct rfb_fence_msg) + length,
		.departure_time = departure_time,
		.arrival_time = now,
	};
	bwe_feed(client->bwe, &sample);

	client->inflight_bytes -= frame_size;

	nvnc_trace("Bandwidth estimate: %.3f Mb/s\n",
			bwe_get_estimate(client->bwe) * 8e-6);

	process_fb_update_requests(client);
}

static int on_client_fence(struct nvnc_client* client)
{
	struct rfb_fence_msg *msg = (struct rfb_fence_msg*)(client->msg_buffer +
			client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint8_t length = msg->length;
	if (client->buffer_len - client->buffer_index < sizeof(*msg) + length)
		return 0;

	if (length > 64) {
		nvnc_log(NVNC_LOG_WARNING,
				"Client sent too long fence message. Closing.");
		nvnc_client_close(client);
		return -1;
	}

	enum rfb_fence_flags flags = ntohl(msg->flags);

	if (flags & RFB_FENCE_REQUEST) {
		if (!on_fence_request(client, flags, msg->payload, length))
			return 0;
	} else {
		on_fence_response(client, flags, msg->payload, length);
	}

	return sizeof(*msg) + length;
}

static int on_client_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	enum rfb_client_to_server_msg_type type =
	        client->msg_buffer[client->buffer_index];

	switch (type) {
	case RFB_CLIENT_TO_SERVER_SET_PIXEL_FORMAT:
		return on_client_set_pixel_format(client);
	case RFB_CLIENT_TO_SERVER_SET_ENCODINGS:
		return on_client_set_encodings(client);
	case RFB_CLIENT_TO_SERVER_FRAMEBUFFER_UPDATE_REQUEST:
		return on_client_fb_update_request(client);
	case RFB_CLIENT_TO_SERVER_KEY_EVENT:
		return on_client_key_event(client);
	case RFB_CLIENT_TO_SERVER_POINTER_EVENT:
		return on_client_pointer_event(client);
	case RFB_CLIENT_TO_SERVER_CLIENT_CUT_TEXT:
		return on_client_cut_text(client);
	case RFB_CLIENT_TO_SERVER_ENABLE_CONTINUOUS_UPDATES:
		return on_client_enable_continuous_updates(client);
	case RFB_CLIENT_TO_SERVER_QEMU:
		return on_client_qemu_event(client);
	case RFB_CLIENT_TO_SERVER_SET_DESKTOP_SIZE:
		return on_client_set_desktop_size_event(client);
	case RFB_CLIENT_TO_SERVER_NTP:
		return on_client_ntp(client);
	case RFB_CLIENT_TO_SERVER_FENCE:
		return on_client_fence(client);
	}

	nvnc_log(NVNC_LOG_WARNING, "Got uninterpretable message from client: %p",
			client);
	nvnc_client_close(client);
	return -1;
}

static int try_read_client_message(struct nvnc_client* client)
{
	if (client->net_stream->state == STREAM_STATE_CLOSED)
		return -1;

	switch (client->state) {
	case VNC_CLIENT_STATE_WAITING_FOR_VERSION:
		return on_version_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_SECURITY:
		return on_security_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_INIT:
		return on_init_message(client);
#ifdef ENABLE_TLS
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION:
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE:
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH:
		return vencrypt_handle_message(client);
#endif
#ifdef HAVE_CRYPTO
	case VNC_CLIENT_STATE_WAITING_FOR_APPLE_DH_RESPONSE:
		return apple_dh_handle_response(client);
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY:
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CHALLENGE:
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CLIENT_HASH:
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CREDENTIALS:
		return rsa_aes_handle_message(client);
#endif
	case VNC_CLIENT_STATE_READY:
		return on_client_message(client);
	}

	nvnc_log(NVNC_LOG_PANIC, "Invalid client state");
	return 0;
}

static void on_client_event(struct stream* stream, enum stream_event event)
{
	struct nvnc_client* client = stream->userdata;

	assert(client->net_stream && client->net_stream == stream);

	if (event == STREAM_EVENT_REMOTE_CLOSED) {
		nvnc_log(NVNC_LOG_INFO, "Client %p hung up", client);
		defer_client_close(client);
		return;
	}

	if (client->cut_text.buffer) {
		process_big_cut_text(client);
		return;
	}

	assert(client->buffer_index == 0);

	void* start = client->msg_buffer + client->buffer_len;
	size_t space = MSG_BUFFER_SIZE - client->buffer_len;
	ssize_t n_read = stream_read(stream, start, space);

	if (n_read == 0)
		return;

	if (n_read < 0) {
		if (errno != EAGAIN) {
			nvnc_log(NVNC_LOG_INFO, "Client connection error: %p",
					client);
			nvnc_client_close(client);
		}

		return;
	}

	client->buffer_len += n_read;

	while (!client->is_blocked_by_fence) {
		client->is_blocked_by_fence =
			client->must_block_after_next_message;
		client->must_block_after_next_message = false;

		int rc = try_read_client_message(client);
		if (rc == 0)
			break;

		if (rc == -1)
			return;

		client->buffer_index += rc;
	}

	if (client->buffer_index > client->buffer_len)
		nvnc_log(NVNC_LOG_PANIC, "Read-buffer index has grown out of bounds");

	client->buffer_len -= client->buffer_index;
	memmove(client->msg_buffer, client->msg_buffer + client->buffer_index,
			client->buffer_len);
	client->buffer_index = 0;
}

static void on_connection(struct aml_handler* poll_handle)
{
	struct nvnc__socket* socket = aml_get_userdata(poll_handle);
	struct nvnc* server = socket->parent;

	struct nvnc_client* client = calloc(1, sizeof(*client));
	if (!client)
		return;

	client->server = server;
	client->quality = 10; /* default to lossless */
	client->led_state = -1; /* trigger sending of initial state */
	client->min_rtt = INT32_MAX;
	client->bwe = bwe_create(INT32_MAX);

	/* default extended clipboard capabilities */
	client->ext_clipboard_caps =
		RFB_EXT_CLIPBOARD_FORMAT_TEXT |
		RFB_EXT_CLIPBOARD_ACTION_REQUEST |
		RFB_EXT_CLIPBOARD_ACTION_NOTIFY |
		RFB_EXT_CLIPBOARD_ACTION_PROVIDE;
	client->ext_clipboard_max_unsolicited_text_size =
		MAX_CLIENT_UNSOLICITED_TEXT_SIZE;

	int fd = accept(socket->fd, NULL, 0);
	if (fd < 0) {
		nvnc_log(NVNC_LOG_WARNING, "Failed to accept a connection");
		goto accept_failure;
	}

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

#ifdef ENABLE_WEBSOCKET
	if (socket->type == NVNC_STREAM_WEBSOCKET)
	{
		client->net_stream = stream_ws_new(fd, on_client_event, client);
	}
	else
#endif
	{
		client->net_stream = stream_new(fd, on_client_event, client);
	}
	if (!client->net_stream) {
		nvnc_log(NVNC_LOG_WARNING, "OOM");
		goto stream_failure;
	}

	if (!server->display->buffer) {
		nvnc_log(NVNC_LOG_WARNING, "No display buffer has been set");
		goto buffer_failure;
	}

	pixman_region_init(&client->damage);

	struct rcbuf* payload = rcbuf_from_string(RFB_VERSION_MESSAGE);
	if (!payload) {
		nvnc_log(NVNC_LOG_WARNING, "OOM");
		goto payload_failure;
	}

	client->last_ping_time = gettime_us(CLOCK_MONOTONIC);
	stream_send(client->net_stream, payload, NULL, NULL);

	LIST_INSERT_HEAD(&server->clients, client, link);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VERSION;

	char ip_address[256];
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	nvnc_client_get_address(client, (struct sockaddr*)&addr, &addrlen);
	sockaddr_to_string(ip_address, sizeof(ip_address),
			(struct sockaddr*)&addr);
	nvnc_log(NVNC_LOG_INFO, "New client connection from %s: %p",
			ip_address, client);

	return;

payload_failure:
	pixman_region_fini(&client->damage);
buffer_failure:
	stream_destroy(client->net_stream);
stream_failure:
	close(fd);
accept_failure:
	free(client);
}

static void sockaddr_to_string(char* dst, size_t sz, const struct sockaddr* addr)
{
	struct sockaddr_in *sa_in = (struct sockaddr_in*)addr;
	struct sockaddr_in6 *sa_in6 = (struct sockaddr_in6*)addr;

	switch (addr->sa_family) {
	case AF_INET:
		inet_ntop(addr->sa_family, &sa_in->sin_addr, dst, sz);
		break;
	case AF_INET6:
		inet_ntop(addr->sa_family, &sa_in6->sin6_addr, dst, sz);
		break;
	default:
		nvnc_log(NVNC_LOG_DEBUG,
				"Don't know how to convert sa_family %d to string",
				addr->sa_family);
		if (sz > 0)
			*dst = 0;
		break;
	}
}

static int bind_address_tcp(const char* name, int port)
{
	struct addrinfo hints = {
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE,
	};

	struct addrinfo* result;

	char service[256];
	snprintf(service, sizeof(service), "%d", port);

	int rc = getaddrinfo(name, service, &hints, &result);
	if (rc != 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to get address info: %s",
				gai_strerror(rc));
		return -1;
	}

	int fd = -1;

	for (struct addrinfo* p = result; p != NULL; p = p->ai_next) {
		char ai_str[256] = { 0 };
		sockaddr_to_string(ai_str, sizeof(ai_str), p->ai_addr);
		nvnc_log(NVNC_LOG_DEBUG, "Trying address: %s", ai_str);

		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			nvnc_log(NVNC_LOG_DEBUG, "Failed to create socket: %m");
			continue;
		}

		int one = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
			nvnc_log(NVNC_LOG_DEBUG, "Failed to set SO_REUSEADDR: %m");
			goto failure;
		}

		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
			nvnc_log(NVNC_LOG_DEBUG, "Successfully bound to address");
			break;
		}

		nvnc_log(NVNC_LOG_DEBUG, "Failed to bind to address: %m");
failure:
		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);
	return fd;
}

static int bind_address_unix(const char* name)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};

	if (strlen(name) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(addr.sun_path, name);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static struct nvnc__socket* nvnc__listen(struct nvnc* self, int fd,
		enum nvnc_stream_type type)
{
	struct nvnc__socket* socket = calloc(1, sizeof(*socket));
	if (!socket)
		return NULL;

	if (listen(fd, 16) < 0)
		goto failure;

	socket->parent = self;
	socket->type = type;
	socket->fd = fd;
	socket->is_external = true;

	socket->poll_handle = aml_handler_new(fd, on_connection, socket, NULL);
	if (!socket->poll_handle) {
		goto failure;
	}

	aml_start(aml_get_default(), socket->poll_handle);

	LIST_INSERT_HEAD(&self->sockets, socket, link);
	return socket;

failure:
	free(socket);
	return NULL;
}

EXPORT
struct nvnc* nvnc_new(void)
{
	nvnc__log_init();
	aml_require_workers(aml_get_default(), -1);

	struct nvnc* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	strcpy(self->name, DEFAULT_NAME);

	LIST_INIT(&self->sockets);
	LIST_INIT(&self->clients);

	return self;
}

EXPORT
int nvnc_listen(struct nvnc* self, int fd, enum nvnc_stream_type type)
{
	struct nvnc__socket* socket = nvnc__listen(self, fd, type);
	return socket ? 0 : -1;
}

EXPORT
int nvnc_listen_tcp(struct nvnc* self, const char* addr, uint16_t port,
		enum nvnc_stream_type type)
{
	int fd = bind_address_tcp(addr, port);
	if (fd < 0)
		return -1;

	struct nvnc__socket* socket = nvnc__listen(self, fd, type);
	if (!socket) {
		close(fd);
		return -1;
	}

	socket->is_external = false;
	return 0;
}

EXPORT
int nvnc_listen_unix(struct nvnc* self, const char* path,
		enum nvnc_stream_type type)
{
	int fd = bind_address_unix(path);
	if (fd < 0)
		return -1;

	struct nvnc__socket* socket = nvnc__listen(self, fd, type);
	if (!socket)
		goto failure;

	socket->is_external = false;
	return 0;

failure:
	if (type == NVNC_STREAM_WEBSOCKET) {
		unlink(path);
	}
	close(fd);

	return -1;
}

EXPORT
struct nvnc* nvnc_open(const char* address, uint16_t port)
{
	struct nvnc* self = nvnc_new();
	if (!self)
		return NULL;

	if (nvnc_listen_tcp(self, address, port, NVNC_STREAM_NORMAL) < 0) {
		nvnc_del(self);
		return NULL;
	}

	return self;
}

EXPORT
struct nvnc* nvnc_open_websocket(const char *address, uint16_t port)
{
#ifdef ENABLE_WEBSOCKET
	struct nvnc* self = nvnc_new();
	if (!self)
		return NULL;

	if (nvnc_listen_tcp(self, address, port, NVNC_STREAM_WEBSOCKET) < 0) {
		nvnc_del(self);
		return NULL;
	}

	return self;
#else
	return NULL;
#endif
}

EXPORT
struct nvnc* nvnc_open_unix(const char* address)
{
	struct nvnc* self = nvnc_new();
	if (!self)
		return NULL;

	if (nvnc_listen_unix(self, address, NVNC_STREAM_NORMAL) < 0) {
		nvnc_del(self);
		return NULL;
	}

	return self;
}

EXPORT
struct nvnc* nvnc_open_from_fd(int fd)
{
	struct nvnc* self = nvnc_new();
	if (!self)
		return NULL;

	if (nvnc_listen(self, fd, NVNC_STREAM_NORMAL) < 0) {
		nvnc_del(self);
		return NULL;
	}

	return self;
}

static void unlink_fd_path(int fd)
{
	struct sockaddr_un addr;
	socklen_t addr_len = sizeof(addr);

	if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
		if (addr.sun_family == AF_UNIX) {
			unlink(addr.sun_path);
		}
	}
}

EXPORT
void nvnc_del(struct nvnc* self)
{
	self->is_closing = true;

	nvnc_cleanup_fn cleanup = self->common.cleanup_fn;
	if (cleanup)
		cleanup(self->common.userdata);

	if (self->display)
		nvnc_display_unref(self->display);

	nvnc_fb_release(self->cursor.buffer);
	nvnc_fb_unref(self->cursor.buffer);
	self->cursor.buffer = NULL;

	// The stream is closed first to stop all communication and to make sure
	// that encoding of new frames does not start.
	struct nvnc_client* client;
	LIST_FOREACH(client, &self->clients, link)
		stream_close(client->net_stream);

	while (!LIST_EMPTY(&self->clients))
		client_close(LIST_FIRST(&self->clients));

	while (!LIST_EMPTY(&self->sockets)) {
		struct nvnc__socket* socket = LIST_FIRST(&self->sockets);
		LIST_REMOVE(socket, link);

		aml_stop(aml_get_default(), socket->poll_handle);
		aml_unref(socket->poll_handle);

		if (!socket->is_external) {
			unlink_fd_path(socket->fd);
		}
		close(socket->fd);

		free(socket);
	}

#ifdef HAVE_CRYPTO
	crypto_rsa_priv_key_del(self->rsa_priv);
	crypto_rsa_pub_key_del(self->rsa_pub);
#endif

#ifdef ENABLE_TLS
	if (self->tls_creds) {
		gnutls_certificate_free_credentials(self->tls_creds);
		gnutls_global_deinit();
	}
#endif

	free(self->ext_clipboard_provide_msg.buffer);

	free(self);
}

EXPORT
void nvnc_close(struct nvnc* self)
{
	nvnc_del(self);
}

static void process_pending_fence(struct nvnc_client* client)
{
	if (client->pending_fence.n_pending_requests == 0) {
		assert(!client->is_blocked_by_fence);
		return;
	}

	if (--client->pending_fence.n_pending_requests != 0)
		return;

	send_fence(client, client->pending_fence.flags,
			client->pending_fence.payload,
			client->pending_fence.length);
	memset(&client->pending_fence, 0, sizeof(client->pending_fence));

	client->is_blocked_by_fence = false;
	on_client_event(client->net_stream, STREAM_EVENT_READ);
}

static void complete_fb_update(struct nvnc_client* client)
{
	if (!client->is_updating)
		return;
	client->is_updating = false;
	process_fb_update_requests(client);
	DTRACE_PROBE1(neatvnc, update_fb_done, client);
}

static void on_write_frame_done(void* userdata, enum stream_req_status status)
{
	struct nvnc_client* client = userdata;
	complete_fb_update(client);
}

static enum rfb_encodings choose_frame_encoding(struct nvnc_client* client,
		const struct nvnc_fb* fb)
{
	for (size_t i = 0; i < client->n_encodings; ++i)
		switch (client->encodings[i]) {
		case RFB_ENCODING_RAW:
		case RFB_ENCODING_TIGHT:
		case RFB_ENCODING_ZRLE:
			return client->encodings[i];
#ifdef ENABLE_OPEN_H264
		case RFB_ENCODING_OPEN_H264:
			// h264 is useless for sw frames
			if (fb->type != NVNC_FB_GBM_BO)
				break;
			if (!have_working_h264_encoder())
				break;
			return client->encodings[i];
#endif
		default:
			break;
		}

	return RFB_ENCODING_RAW;
}

static bool client_has_encoding(const struct nvnc_client* client,
		enum rfb_encodings encoding)
{
	for (size_t i = 0; i < client->n_encodings; ++i)
		if (client->encodings[i] == encoding)
			return true;

	return false;
}

static void finish_fb_update(struct nvnc_client* client,
		struct encoded_frame* frame)
{
	if (client->net_stream->state == STREAM_STATE_CLOSED)
		goto complete;

	if (client->formats_changed &&
			!client_has_encoding(client, RFB_ENCODING_FENCE)) {
		/* Client has requested new pixel format or encoding in the
		 * meantime, so it probably won't know what to do with this
		 * frame. Pending requests get incremented because this one is
		 * dropped.
		 */
		nvnc_log(NVNC_LOG_DEBUG, "Client changed pixel format or encoding with in-flight buffer");
		client->n_pending_requests++;
		goto complete;
	}

	DTRACE_PROBE2(neatvnc, send_fb_start, client, pts);
	frame->n_rects += will_send_pts(client, frame->pts) ? 1 : 0;

	bool is_resized = client->known_width != frame->width ||
		client->known_height != frame->height;

	if (is_resized) {
		frame->n_rects += 1;

		if (!client_supports_resizing(client)) {
			nvnc_log(NVNC_LOG_ERROR, "Display has been resized but client does not support resizing.  Closing.");
			client_close(client);
			return;
		}
	}

	struct rfb_server_fb_update_msg update_msg = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(frame->n_rects),
	};
	if (stream_write(client->net_stream, &update_msg,
			sizeof(update_msg), NULL, NULL) < 0)
		goto complete;

	if (is_resized && send_desktop_resize_rect(client, frame->width,
				frame->height) < 0)
		goto complete;

	if (send_pts_rect(client, frame->pts) < 0)
		goto complete;

	encoded_frame_ref(frame);
	if (stream_send(client->net_stream, &frame->buf, on_write_frame_done,
				client) < 0)
		goto complete;

	send_ping(client, frame->buf.size);

	process_pending_fence(client);

	DTRACE_PROBE2(neatvnc, send_fb_done, client, pts);
	return;

complete:
	complete_fb_update(client);
}

static void on_encode_frame_done(struct encoder* encoder,
		struct encoded_frame* result)
{
	struct nvnc_client* client = encoder->userdata;
	client->encoder->on_done = NULL;
	client->encoder->userdata = NULL;
	finish_fb_update(client, result);
}

static bool client_supports_resizing(const struct nvnc_client* client)
{
	return client_has_encoding(client, RFB_ENCODING_DESKTOPSIZE) ||
		client_has_encoding(client, RFB_ENCODING_EXTENDEDDESKTOPSIZE);
}

static int send_desktop_resize_rect(struct nvnc_client* client, uint16_t width,
		uint16_t height)
{
	client->known_width = width;
	client->known_height = height;

	pixman_region_union_rect(&client->damage, &client->damage, 0, 0,
			width, height);

	if (client_has_encoding(client, RFB_ENCODING_EXTENDEDDESKTOPSIZE)) {
		send_extended_desktop_size_rect(client, width, height,
				RFB_RESIZE_INITIATOR_SERVER,
				RFB_RESIZE_STATUS_SUCCESS);
		return 0;
	}

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_DESKTOPSIZE),
		.width = htons(width),
		.height = htons(height),
	};

	stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	return 0;
}

static bool send_ext_support_frame(struct nvnc_client* client)
{
	int has_qemu_ext =
		client_has_encoding(client, RFB_ENCODING_QEMU_EXT_KEY_EVENT);
	int has_ntp = client_has_encoding(client, RFB_ENCODING_NTP);
	int has_ext_mouse_buttons =
		client_has_encoding(client, RFB_ENCODING_EXT_MOUSE_BUTTONS);
	int n_rects = has_qemu_ext + has_ntp + has_ext_mouse_buttons;
	if (n_rects == 0)
		return false;

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(n_rects),
	};
	stream_write(client->net_stream, &head, sizeof(head), NULL, NULL);

	if (has_qemu_ext) {
		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_QEMU_EXT_KEY_EVENT),
		};
		stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	}

	if (has_ntp) {
		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_NTP),
		};
		stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	}

	if (has_ext_mouse_buttons) {
		struct rfb_server_fb_rect rect = {
			.encoding = htonl(RFB_ENCODING_EXT_MOUSE_BUTTONS),
		};
		stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
		client->has_ext_mouse_buttons = true;
	}

	return true;
}

void nvnc__damage_region(struct nvnc* self, const struct pixman_region16* damage)
{
	struct nvnc_client* client;

	LIST_FOREACH(client, &self->clients, link)
		if (client->net_stream->state != STREAM_STATE_CLOSED)
			pixman_region_union(&client->damage, &client->damage,
					(struct pixman_region16*)damage);

	LIST_FOREACH(client, &self->clients, link)
		process_fb_update_requests(client);
}

EXPORT
void nvnc_set_userdata(void* self, void* userdata, nvnc_cleanup_fn cleanup_fn)
{
	struct nvnc_common* common = self;
	common->userdata = userdata;
	common->cleanup_fn = cleanup_fn;
}

EXPORT
void* nvnc_get_userdata(const void* self)
{
	const struct nvnc_common* common = self;
	return common->userdata;
}

EXPORT
void nvnc_set_key_fn(struct nvnc* self, nvnc_key_fn fn)
{
	self->key_fn = fn;
}

EXPORT
void nvnc_set_key_code_fn(struct nvnc* self, nvnc_key_fn fn)
{
	self->key_code_fn = fn;
}

EXPORT
void nvnc_set_pointer_fn(struct nvnc* self, nvnc_pointer_fn fn)
{
	self->pointer_fn = fn;
}

EXPORT
void nvnc_set_fb_req_fn(struct nvnc* self, nvnc_fb_req_fn fn)
{
	self->fb_req_fn = fn;
}

EXPORT
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn fn)
{
	self->new_client_fn = fn;
}

EXPORT
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn)
{
	self->cleanup_fn = fn;
}

EXPORT
void nvnc_set_cut_text_fn(struct nvnc* self, nvnc_cut_text_fn fn)
{
	self->cut_text_fn = fn;
}

EXPORT
void nvnc_set_desktop_layout_fn(struct nvnc* self, nvnc_desktop_layout_fn fn)
{
	self->desktop_layout_fn = fn;
}

EXPORT
void nvnc_add_display(struct nvnc* self, struct nvnc_display* display)
{
	if (self->display) {
		nvnc_log(NVNC_LOG_PANIC, "Multiple displays are not implemented. Aborting!");
	}

	display->server = self;
	self->display = display;
	nvnc_display_ref(display);
}

EXPORT
void nvnc_remove_display(struct nvnc* self, struct nvnc_display* display)
{
	if (self->display != display)
		return;

	nvnc_display_unref(display);
	self->display = NULL;
}

EXPORT
struct nvnc* nvnc_client_get_server(const struct nvnc_client* client)
{
	return client->server;
}

EXPORT
int nvnc_client_get_address(const struct nvnc_client* client,
		struct sockaddr* restrict addr, socklen_t* restrict addrlen) {
	return getpeername(client->net_stream->fd, addr, addrlen);
}

EXPORT
const char* nvnc_client_get_auth_username(const struct nvnc_client* client) {
	if (client->username[0] == '\0')
		return NULL;
	return client->username;
}

EXPORT
struct nvnc_client* nvnc_client_first(struct nvnc* self)
{
	return LIST_FIRST(&self->clients);
}

EXPORT
struct nvnc_client* nvnc_client_next(struct nvnc_client* client)
{
	assert(client);
	return LIST_NEXT(client, link);
}

EXPORT
void nvnc_client_close(struct nvnc_client* client)
{
	client_close(client);
}

EXPORT
bool nvnc_client_supports_cursor(const struct nvnc_client* client)
{
	for (size_t i = 0; i < client->n_encodings; ++i) {
		if (client->encodings[i] == RFB_ENCODING_CURSOR)
			return true;
	}
	return false;
}

static bool client_send_led_state(struct nvnc_client* client)
{
	if (client->pending_led_state == client->led_state)
		return false;

	bool have_qemu_led_state =
		client_has_encoding(client, RFB_ENCODING_QEMU_LED_STATE);
	bool have_vmware_led_state =
		client_has_encoding(client, RFB_ENCODING_VMWARE_LED_STATE);

	if (!have_qemu_led_state && !have_vmware_led_state)
		return false;

	nvnc_log(NVNC_LOG_DEBUG, "Keyboard LED state changed: %x -> %x",
			client->led_state, client->pending_led_state);

	struct vec payload;
	vec_init(&payload, 4096);

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_QEMU_LED_STATE),
	};

	vec_append(&payload, &head, sizeof(head));
	vec_append(&payload, &rect, sizeof(rect));

	if (have_qemu_led_state) {
		uint8_t data = client->pending_led_state;
		vec_append(&payload, &data, sizeof(data));
	} else if (have_vmware_led_state) {
		uint32_t data = htonl(client->pending_led_state);
		vec_append(&payload, &data, sizeof(data));
	}

	stream_send(client->net_stream, rcbuf_new(payload.data, payload.len),
			NULL, NULL);
	client->led_state = client->pending_led_state;

	return true;
}

EXPORT
void nvnc_client_set_led_state(struct nvnc_client* client,
		enum nvnc_keyboard_led_state state)
{
	client->pending_led_state = state;
	process_fb_update_requests(client);
}

EXPORT
void nvnc_set_name(struct nvnc* self, const char* name)
{
	strncpy(self->name, name, sizeof(self->name));
	self->name[sizeof(self->name) - 1] = '\0';
}

EXPORT
bool nvnc_has_auth(void)
{
#if defined(ENABLE_TLS) || defined(HAVE_CRYPTO)
	return true;
#else
	return false;
#endif
}

EXPORT
int nvnc_set_tls_creds(struct nvnc* self, const char* privkey_path,
		const char* cert_path)
{
#ifdef ENABLE_TLS
	if (self->tls_creds)
		return -1;

	/* Note: This is globally reference counted, so we don't need to worry
	 * about messing with other libraries.
	 */
	int rc = gnutls_global_init();
	if (rc != GNUTLS_E_SUCCESS) {
		nvnc_log(NVNC_LOG_ERROR, "GnuTLS: Failed to initialise: %s",
				gnutls_strerror(rc));
		return -1;
	}

	rc = gnutls_certificate_allocate_credentials(&self->tls_creds);
	if (rc != GNUTLS_E_SUCCESS) {
		nvnc_log(NVNC_LOG_ERROR, "GnuTLS: Failed to allocate credentials: %s",
				gnutls_strerror(rc));
		goto cert_alloc_failure;
	}

	rc = gnutls_certificate_set_x509_key_file(
			self->tls_creds, cert_path, privkey_path, GNUTLS_X509_FMT_PEM);
	if (rc != GNUTLS_E_SUCCESS) {
		nvnc_log(NVNC_LOG_ERROR, "GnuTLS: Failed to load credentials: %s",
				gnutls_strerror(rc));
		goto cert_set_failure;
	}

	return 0;

cert_set_failure:
	gnutls_certificate_free_credentials(self->tls_creds);
	self->tls_creds = NULL;
cert_alloc_failure:
	gnutls_global_deinit();
#endif
	return -1;
}

EXPORT
int nvnc_enable_auth(struct nvnc* self, enum nvnc_auth_flags flags,
		nvnc_auth_fn auth_fn, void* userdata)
{
#if defined(ENABLE_TLS) || defined(HAVE_CRYPTO)
	self->auth_flags = flags;
	self->auth_fn = auth_fn;
	self->auth_ud = userdata;
	return 0;
#endif
	return -1;
}

static bool buffers_are_equal(struct nvnc_fb* a, struct nvnc_fb* b)
{
	if (a == b)
		return true;

	if ((a && !b) || (!a && b))
		return false;

	if (nvnc_fb_get_width(a) != nvnc_fb_get_width(b) ||
			nvnc_fb_get_height(a) != nvnc_fb_get_height(b) ||
			nvnc_fb_get_stride(a) != nvnc_fb_get_stride(b) ||
			nvnc_fb_get_pixel_size(a) != nvnc_fb_get_pixel_size(b) ||
			nvnc_fb_get_fourcc_format(a) != nvnc_fb_get_fourcc_format(b) ||
			nvnc_fb_get_transform(a) != nvnc_fb_get_transform(b))
		return false;

	nvnc_fb_map(a);
	nvnc_fb_map(b);

	const uint8_t* data_a = nvnc_fb_get_addr(a);
	const uint8_t* data_b = nvnc_fb_get_addr(b);

	uint32_t size = nvnc_fb_get_stride(a) * nvnc_fb_get_pixel_size(a) *
		nvnc_fb_get_height(a);
	bool result = true;

	for (uint32_t i = 0; i < size; ++i)
		result &= data_a[i] == data_b[i];

	return result;
}

EXPORT
void nvnc_set_cursor(struct nvnc* self, struct nvnc_fb* fb, uint16_t width,
		uint16_t height, uint16_t hotspot_x, uint16_t hotspot_y,
		bool is_damaged)
{
	bool should_send = is_damaged && !buffers_are_equal(self->cursor.buffer, fb);

	nvnc_fb_release(self->cursor.buffer);
	nvnc_fb_unref(self->cursor.buffer);

	self->cursor.buffer = fb;
	self->cursor.width = width;
	self->cursor.height = height;
	self->cursor.hotspot_x = hotspot_x;
	self->cursor.hotspot_y = hotspot_y;

	if (fb) {
		nvnc_fb_ref(fb);
		nvnc_fb_hold(fb);
	}

	if (!should_send)
		return;

	self->cursor_seq++;

	struct nvnc_client* client;
	LIST_FOREACH(client, &self->clients, link)
		process_fb_update_requests(client);
}

EXPORT
int nvnc_set_rsa_creds(struct nvnc* self, const char* path)
{
#ifdef HAVE_CRYPTO
	crypto_rsa_priv_key_del(self->rsa_priv);
	crypto_rsa_pub_key_del(self->rsa_pub);

	self->rsa_priv = crypto_rsa_priv_key_new();
	self->rsa_pub = crypto_rsa_pub_key_new();

	bool ok = crypto_rsa_priv_key_load(self->rsa_priv, self->rsa_pub, path);
	return ok ? 0 : -1;
#endif
	return -1;
}

static uint32_t find_highest_client_depth(const struct nvnc* self)
{
	int max_depth = 0;

	struct nvnc_client* client;
	LIST_FOREACH(client, &self->clients, link) {
		int depth = rfb_pixfmt_depth(&client->pixfmt);
		if (depth > max_depth)
			max_depth = depth;
	}

	return max_depth != 0 ? max_depth : 24;
}

// TODO: Give linear a higher rating if we have a v4l2m2m based encoder
// TODO: Disable v4l2m2m based encoders while the format is not linear
// TODO: Notify when rating criteria change

EXPORT
double nvnc_rate_pixel_format(const struct nvnc* self,
		enum nvnc_fb_type fb_type, uint32_t format, uint64_t modifier)
{
	if (fb_type == NVNC_FB_SIMPLE && modifier) {
		nvnc_log(NVNC_LOG_ERROR, "modifier should be 0 for simple buffers");
		return 0;
	}
	int max_depth = find_highest_client_depth(self);
	return rate_pixel_format(format, modifier, 0, max_depth);
}

EXPORT
double nvnc_rate_cursor_pixel_format(const struct nvnc* self,
		enum nvnc_fb_type fb_type, uint32_t format, uint64_t modifier)
{
	if (fb_type == NVNC_FB_SIMPLE && modifier) {
		nvnc_log(NVNC_LOG_ERROR, "modifier should be 0 for simple buffers");
		return 0;
	}
	int max_depth = find_highest_client_depth(self);
	return rate_pixel_format(format, modifier, FORMAT_RATING_NEED_ALPHA,
			max_depth);
}
