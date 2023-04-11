/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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
#include "stream.h"
#include "config.h"
#include "usdt.h"
#include "encoder.h"
#include "enc-util.h"
#include "cursor.h"
#include "logging.h"

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

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

#ifndef DRM_FORMAT_INVALID
#define DRM_FORMAT_INVALID 0
#endif

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR DRM_FORMAT_MOD_NONE
#endif

#define DEFAULT_NAME "Neat VNC"

#define EXPORT __attribute__((visibility("default")))

static int send_desktop_resize(struct nvnc_client* client, struct nvnc_fb* fb);
static int send_qemu_key_ext_frame(struct nvnc_client* client);
static enum rfb_encodings choose_frame_encoding(struct nvnc_client* client,
		struct nvnc_fb*);
static void on_encode_frame_done(struct encoder*, struct rcbuf*, uint64_t pts);
static bool client_has_encoding(const struct nvnc_client* client,
		enum rfb_encodings encoding);
static void process_fb_update_requests(struct nvnc_client* client);

#if defined(GIT_VERSION)
EXPORT const char nvnc_version[] = GIT_VERSION;
#elif defined(PROJECT_VERSION)
EXPORT const char nvnc_version[] = PROJECT_VERSION;
#else
EXPORT const char nvnc_version[] = "UNKNOWN";
#endif

extern const unsigned short code_map_qnum_to_linux[];

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

static void client_close(struct nvnc_client* client)
{
	nvnc_log(NVNC_LOG_INFO, "Closing client connection %p: ref %d", client,
			client->ref);

	nvnc_cleanup_fn cleanup = client->common.cleanup_fn;
	if (cleanup)
		cleanup(client->common.userdata);

	nvnc_client_fn fn = client->cleanup_fn;
	if (fn)
		fn(client);

	if (client->current_fb) {
		nvnc_fb_release(client->current_fb);
		nvnc_fb_unref(client->current_fb);
	}

	LIST_REMOVE(client, link);
	stream_destroy(client->net_stream);
	if (client->encoder) {
		client->server->n_damage_clients -=
			!(client->encoder->impl->flags &
					ENCODER_IMPL_FLAG_IGNORES_DAMAGE);
		client->encoder->on_done = NULL;
	}
	encoder_unref(client->encoder);
	pixman_region_fini(&client->damage);
	free(client->cut_text.buffer);
	free(client);
}

static inline void client_unref(struct nvnc_client* client)
{
	assert(client->ref > 0);

	if (--client->ref == 0)
		client_close(client);
}

static inline void client_ref(struct nvnc_client* client)
{
	++client->ref;
}

static void close_after_write(void* userdata, enum stream_req_status status)
{
	struct nvnc_client* client = userdata;
	nvnc_log(NVNC_LOG_DEBUG, "close_after_write(%p): ref %d", client,
			client->ref);
	nvnc_client_close(client);
}

static int handle_unsupported_version(struct nvnc_client* client)
{
	char buffer[256];

	client->state = VNC_CLIENT_STATE_ERROR;

	struct rfb_error_reason* reason = (struct rfb_error_reason*)(buffer + 1);

	static const char reason_string[] = "Unsupported version\n";

	buffer[0] = 0; /* Number of security types is 0 on error */
	reason->length = htonl(strlen(reason_string));
	strcpy(reason->message, reason_string);

	size_t len = 1 + sizeof(*reason) + strlen(reason_string);
	stream_write(client->net_stream, buffer, len, close_after_write,
	             client);

	return 0;
}

static int on_version_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 12)
		return 0;

	char version_string[13];
	memcpy(version_string, client->msg_buffer + client->buffer_index, 12);
	version_string[12] = '\0';

	if (strcmp(RFB_VERSION_MESSAGE, version_string) != 0)
		return handle_unsupported_version(client);

	struct rfb_security_types_msg security = { 0 };
	security.n = 1;
	security.types[0] = RFB_SECURITY_TYPE_NONE;

#ifdef ENABLE_TLS
	if (client->server->auth_fn)
		security.types[0] = RFB_SECURITY_TYPE_VENCRYPT;
#endif

	stream_write(client->net_stream, &security, sizeof(security), NULL,
	             NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_SECURITY;
	return 12;
}

static int security_handshake_failed(struct nvnc_client* client,
                                     const char* reason_string)
{
	char buffer[256];

	client->state = VNC_CLIENT_STATE_ERROR;

	uint8_t* result = (uint8_t*)buffer;

	struct rfb_error_reason* reason =
	        (struct rfb_error_reason*)(buffer + sizeof(*result));

	*result = htonl(RFB_SECURITY_HANDSHAKE_FAILED);
	reason->length = htonl(strlen(reason_string));
	(void)strcmp(reason->message, reason_string);

	size_t len = sizeof(*result) + sizeof(*reason) + strlen(reason_string);
	stream_write(client->net_stream, buffer, len, close_after_write,
	             client);

	return 0;
}

static int security_handshake_ok(struct nvnc_client* client)
{
	uint32_t result = htonl(RFB_SECURITY_HANDSHAKE_OK);
	return stream_write(client->net_stream, &result, sizeof(result), NULL,
	                    NULL);
}

#ifdef ENABLE_TLS
static int send_byte(struct nvnc_client* client, uint8_t value)
{
	return stream_write(client->net_stream, &value, 1, NULL, NULL);
}

static int send_byte_and_close(struct nvnc_client* client, uint8_t value)
{
	return stream_write(client->net_stream, &value, 1, close_after_write,
	                    client);
}

static int vencrypt_send_version(struct nvnc_client* client)
{
	struct rfb_vencrypt_version_msg msg = {
		.major = 0,
		.minor = 2,
	};

	return stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
}

static int on_vencrypt_version_message(struct nvnc_client* client)
{
	struct rfb_vencrypt_version_msg* msg =
		(struct rfb_vencrypt_version_msg*)&client->msg_buffer[client->buffer_index];

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	if (msg->major != 0 || msg->minor != 2) {
		security_handshake_failed(client, "Unsupported VeNCrypt version");
		return sizeof(*msg);
	}

	send_byte(client, 0);

	struct rfb_vencrypt_subtypes_msg result = { .n = 1, };
	result.types[0] = htonl(RFB_VENCRYPT_X509_PLAIN);

	stream_write(client->net_stream, &result, sizeof(result), NULL, NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE;

	return sizeof(*msg);
}

static int on_vencrypt_subtype_message(struct nvnc_client* client)
{
	uint32_t* msg = (uint32_t*)&client->msg_buffer[client->buffer_index];

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	enum rfb_vencrypt_subtype subtype = ntohl(*msg);

	if (subtype != RFB_VENCRYPT_X509_PLAIN) {
		client->state = VNC_CLIENT_STATE_ERROR;
		send_byte_and_close(client, 0);
		return sizeof(*msg);
	}

	send_byte(client, 1);

	if (stream_upgrade_to_tls(client->net_stream, client->server->tls_creds) < 0) {
		client->state = VNC_CLIENT_STATE_ERROR;
		nvnc_client_close(client);
		return sizeof(*msg);
	}

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH;

	return sizeof(*msg);
}

static int on_vencrypt_plain_auth_message(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_vencrypt_plain_auth_msg* msg =
	        (void*)(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint32_t ulen = ntohl(msg->username_len);
	uint32_t plen = ntohl(msg->password_len);

	if (client->buffer_len - client->buffer_index < sizeof(*msg) + ulen + plen)
		return 0;

	char username[256];
	char password[256];

	memcpy(username, msg->text, MIN(ulen, sizeof(username) - 1));
	memcpy(password, msg->text + ulen, MIN(plen, sizeof(password) - 1));

	username[MIN(ulen, sizeof(username) - 1)] = '\0';
	password[MIN(plen, sizeof(password) - 1)] = '\0';

	strncpy(client->username, username, sizeof(client->username));
	client->username[sizeof(client->username) - 1] = '\0';

	if (server->auth_fn(username, password, server->auth_ud)) {
		nvnc_log(NVNC_LOG_INFO, "User \"%s\" authenticated", username);
		security_handshake_ok(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		nvnc_log(NVNC_LOG_INFO, "User \"%s\" rejected", username);
		security_handshake_failed(client, "Invalid username or password");
	}

	return sizeof(*msg) + ulen + plen;
}
#endif

static int on_security_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t type = client->msg_buffer[client->buffer_index];

	switch (type) {
	case RFB_SECURITY_TYPE_NONE:
		security_handshake_ok(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
		break;
#ifdef ENABLE_TLS
	case RFB_SECURITY_TYPE_VENCRYPT:
		vencrypt_send_version(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION;
		break;
#endif
	default:
		security_handshake_failed(client, "Unsupported security type");
		break;
	}

	return sizeof(type);
}

static void disconnect_all_other_clients(struct nvnc_client* client)
{
	struct nvnc_client* node;
	struct nvnc_client* tmp;

	LIST_FOREACH_SAFE (node, &client->server->clients, link, tmp)
		if (node != client) {
			nvnc_log(NVNC_LOG_DEBUG,
					"disconnect other client %p (ref %d)",
					node, node->ref);
			nvnc_client_close(node);
		}

}

static void send_server_init_message(struct nvnc_client* client)
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

	struct rfb_server_init_msg* msg = calloc(1, size);
	if (!msg)
		goto close;

	msg->width = htons(width);
	msg->height = htons(height);
	msg->name_length = htonl(name_len);
	memcpy(msg->name_string, server->name, name_len);

	int rc = rfb_pixfmt_from_fourcc(&msg->pixel_format, fourcc);
	if (rc < 0)
		goto pixfmt_failure;

	msg->pixel_format.red_max = htons(msg->pixel_format.red_max);
	msg->pixel_format.green_max = htons(msg->pixel_format.green_max);
	msg->pixel_format.blue_max = htons(msg->pixel_format.blue_max);

	struct rcbuf* payload = rcbuf_new(msg, size);
	stream_send(client->net_stream, payload, NULL, NULL);

	client->known_width = width;
	client->known_height = height;
	return;

pixfmt_failure:
	free(msg);
close:
	nvnc_client_close(client);
}

static int on_init_message(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t shared_flag = client->msg_buffer[client->buffer_index];
	if (!shared_flag)
		disconnect_all_other_clients(client);

	send_server_init_message(client);

	nvnc_client_fn fn = client->server->new_client_fn;
	if (fn)
		fn(client);

	client->state = VNC_CLIENT_STATE_READY;
	return sizeof(shared_flag);
}

static int on_client_set_pixel_format(struct nvnc_client* client)
{
	if (client->buffer_len - client->buffer_index <
	    4 + sizeof(struct rfb_pixel_format))
		return 0;

	struct rfb_pixel_format* fmt =
	        (struct rfb_pixel_format*)(client->msg_buffer +
	                                   client->buffer_index + 4);

	if (!fmt->true_colour_flag) {
		/* We don't really know what to do with color maps right now */
		nvnc_client_close(client);
		return 0;
	}

	fmt->red_max = ntohs(fmt->red_max);
	fmt->green_max = ntohs(fmt->green_max);
	fmt->blue_max = ntohs(fmt->blue_max);

	memcpy(&client->pixfmt, fmt, sizeof(client->pixfmt));

	client->has_pixfmt = true;

	return 4 + sizeof(struct rfb_pixel_format);
}

static int on_client_set_encodings(struct nvnc_client* client)
{
	struct rfb_client_set_encodings_msg* msg =
	        (struct rfb_client_set_encodings_msg*)(client->msg_buffer +
	                                               client->buffer_index);

	size_t n_encodings = MIN(MAX_ENCODINGS, ntohs(msg->n_encodings));
	size_t n = 0;

	if (client->buffer_len - client->buffer_index <
	    sizeof(*msg) + n_encodings * 4)
		return 0;

	client->quality = 10;

	for (size_t i = 0; i < n_encodings; ++i) {
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
		case RFB_ENCODING_PTS:
		case RFB_ENCODING_NTP:
			client->encodings[n++] = encoding;
		}

		if (RFB_ENCODING_JPEG_LOWQ <= encoding &&
				encoding <= RFB_ENCODING_JPEG_HIGHQ)
			client->quality = encoding - RFB_ENCODING_JPEG_LOWQ;
	}

	client->n_encodings = n;

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
	case RFB_ENCODING_TIGHT: return "tight";
	case RFB_ENCODING_ZRLE: return "zrle";
	case RFB_ENCODING_OPEN_H264: return "open-h264";
	default:
		break;
	}

	return "UNKNOWN";
}

static void process_fb_update_requests(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (!server->display || !server->display->buffer)
		return;

	if (client->net_stream->state == STREAM_STATE_CLOSED)
		return;

	if (client->is_updating || client->n_pending_requests == 0)
		return;

	struct nvnc_fb* fb = client->server->display->buffer;
	assert(fb);

	if (!client->has_pixfmt) {
		rfb_pixfmt_from_fourcc(&client->pixfmt, fb->fourcc_format);
		client->has_pixfmt = true;
	}

	if (fb->width != client->known_width
	    || fb->height != client->known_height) {
		send_desktop_resize(client, fb);

		if (--client->n_pending_requests <= 0)
			return;
	}

	if (server->key_code_fn && !client->is_qemu_key_ext_notified
	    && client_has_encoding(client, RFB_ENCODING_QEMU_EXT_KEY_EVENT)) {
		send_qemu_key_ext_frame(client);
		client->is_qemu_key_ext_notified = true;

		if (--client->n_pending_requests <= 0)
			return;
	}

	if (server->cursor_seq != client->cursor_seq
			&& client_has_encoding(client, RFB_ENCODING_CURSOR)) {
		send_cursor_update(client);

		if (--client->n_pending_requests <= 0)
			return;
	}

	if (!pixman_region_not_empty(&client->damage))
		return;

	DTRACE_PROBE1(neatvnc, update_fb_start, client);

	enum rfb_encodings encoding = choose_frame_encoding(client, fb);
	if (!client->encoder || encoding != encoder_get_type(client->encoder)) {
		int width = server->display->buffer->width;
		int height = server->display->buffer->height;
		if (client->encoder) {
			server->n_damage_clients -=
				!(client->encoder->impl->flags &
						ENCODER_IMPL_FLAG_IGNORES_DAMAGE);
			client->encoder->on_done = NULL;
		}
		encoder_unref(client->encoder);
		client->encoder = encoder_new(encoding, width, height);
		if (!client->encoder) {
			nvnc_log(NVNC_LOG_ERROR, "Failed to allocate new encoder");
			return;
		}

		server->n_damage_clients +=
			!(client->encoder->impl->flags &
					ENCODER_IMPL_FLAG_IGNORES_DAMAGE);

		nvnc_log(NVNC_LOG_INFO, "Choosing %s encoding for client %p",
				encoding_to_string(encoding), client);
	}

	/* The client's damage is exchanged for an empty one */
	struct pixman_region16 damage = client->damage;
	pixman_region_init(&client->damage);

	client->is_updating = true;
	client->current_fb = fb;
	nvnc_fb_hold(fb);
	nvnc_fb_ref(fb);

	client_ref(client);

	encoder_set_quality(client->encoder, client->quality);
	encoder_set_output_format(client->encoder, &client->pixfmt);

	client->encoder->on_done = on_encode_frame_done;
	client->encoder->userdata = client;

	DTRACE_PROBE2(neatvnc, process_fb_update_requests__encode,
			client, fb->pts);

	if (encoder_encode(client->encoder, fb, &damage) >= 0) {
		--client->n_pending_requests;
	} else {
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode current frame");
		client_unref(client);
		client->is_updating = false;
		assert(client->current_fb);
		nvnc_fb_release(client->current_fb);
		nvnc_fb_unref(client->current_fb);
		client->current_fb = NULL;
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

	uint32_t evdev_keycode = code_map_qnum_to_linux[xt_keycode];
	if (!evdev_keycode)
		evdev_keycode = xt_keycode;

	nvnc_key_fn fn = server->key_code_fn;
	if (fn)
		fn(client, evdev_keycode, !!down_flag);

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

	nvnc_log(NVNC_LOG_WARNING, "Got uninterpretable qemu message from client: %p (ref %d)",
	          client, client->ref);
	nvnc_client_close(client);
	return 0;
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
	uint16_t x = ntohs(msg->x);
	uint16_t y = ntohs(msg->y);

	nvnc_pointer_fn fn = server->pointer_fn;
	if (fn)
		fn(client, x, y, button_mask);

	return sizeof(*msg);
}

EXPORT
void nvnc_send_cut_text(struct nvnc* server, const char* text, uint32_t len)
{
	struct rfb_cut_text_msg msg;

	msg.type = RFB_SERVER_TO_CLIENT_SERVER_CUT_TEXT;
	msg.length = htonl(len);

	struct nvnc_client* client;
	LIST_FOREACH (client, &server->clients, link) {
		stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
		stream_write(client->net_stream, text, len, NULL, NULL);
	}
}

static int on_client_cut_text(struct nvnc_client* client)
{
	struct rfb_cut_text_msg* msg =
	        (struct rfb_cut_text_msg*)(client->msg_buffer +
	                                   client->buffer_index);

	size_t left_to_process = client->buffer_len - client->buffer_index;

	if (left_to_process < sizeof(*msg))
		return 0;

	uint32_t length = ntohl(msg->length);
	uint32_t max_length = MAX_CUT_TEXT_SIZE;

	/* Messages greater than this size are unsupported */
	if (length > max_length) {
		nvnc_log(NVNC_LOG_ERROR, "Copied text length (%d) is greater than max supported length (%d)",
			length, max_length);
		nvnc_client_close(client);
		return 0;
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
		return 0;
	}

	size_t partial_size = left_to_process - sizeof(*msg);

	memcpy(client->cut_text.buffer, msg->text, partial_size);

	client->cut_text.length = length;
	client->cut_text.index = partial_size;

	return left_to_process;
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
			nvnc_log(NVNC_LOG_INFO, "Client connection error: %p (ref %d)",
				  client, client->ref);
			nvnc_client_close(client);
		}

		return;
	}

	client->cut_text.index += n_read;

	if (client->cut_text.index != client->cut_text.length)
		return;

	nvnc_cut_text_fn fn = client->server->cut_text_fn;
	if (fn)
		fn(client, client->cut_text.buffer, client->cut_text.length);

	free(client->cut_text.buffer);
	client->cut_text.buffer = NULL;
}

static enum rfb_resize_status check_desktop_layout(struct nvnc_client* client,
		uint16_t width, uint16_t height, uint8_t n_screens,
		struct rfb_screen* screens)
{
	struct nvnc* server = client->server;
	struct nvnc_desktop_layout* layout;
	enum rfb_resize_status status = RFB_RESIZE_STATUS_SUCCESS;

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

static void send_extended_desktop_size(struct nvnc_client* client,
		enum rfb_resize_initiator initiator,
		enum rfb_resize_status status)
{
	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_EXTENDEDDESKTOPSIZE),
		.x = htons(initiator),
		.y = htons(status),
		.width = htons(client->known_width),
		.height = htons(client->known_height),
	};

	uint8_t number_of_screens = 1;
	uint8_t buf[4] = { number_of_screens };

	struct rfb_screen screen = {
		.width = htons(client->known_width),
		.height = htons(client->known_height),
	};

	stream_write(client->net_stream, &head, sizeof(head), NULL, NULL);
	stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	stream_write(client->net_stream, &buf, sizeof(buf), NULL, NULL);
	stream_write(client->net_stream, &screen, sizeof(screen), NULL, NULL);
}

static int on_client_set_desktop_size_event(struct nvnc_client* client)
{
	struct rfb_client_set_desktop_size_event_msg* msg;
	enum rfb_resize_status status;
	uint16_t width, height;

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	msg = (struct rfb_client_set_desktop_size_event_msg*)
	      (client->msg_buffer + client->buffer_index);

	width = ntohs(msg->width);
	height = ntohs(msg->height);

	status = check_desktop_layout(client, width, height,
			msg->number_of_screens, msg->screens);

	send_extended_desktop_size(client, RFB_RESIZE_INITIATOR_THIS_CLIENT,
				   status);

	return sizeof(*msg) + msg->number_of_screens * sizeof(struct rfb_screen);
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
	case RFB_CLIENT_TO_SERVER_QEMU:
		return on_client_qemu_event(client);
	case RFB_CLIENT_TO_SERVER_SET_DESKTOP_SIZE:
		return on_client_set_desktop_size_event(client);
	case RFB_CLIENT_TO_SERVER_NTP:
		return on_client_ntp(client);
	}

	nvnc_log(NVNC_LOG_WARNING, "Got uninterpretable message from client: %p (ref %d)",
	          client, client->ref);
	nvnc_client_close(client);
	return 0;
}

static int try_read_client_message(struct nvnc_client* client)
{
	switch (client->state) {
	case VNC_CLIENT_STATE_ERROR:
		return client->buffer_len - client->buffer_index;
	case VNC_CLIENT_STATE_WAITING_FOR_VERSION:
		return on_version_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_SECURITY:
		return on_security_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_INIT:
		return on_init_message(client);
#ifdef ENABLE_TLS
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION:
		return on_vencrypt_version_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE:
		return on_vencrypt_subtype_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH:
		return on_vencrypt_plain_auth_message(client);
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
		nvnc_log(NVNC_LOG_INFO, "Client %p (%d) hung up", client, client->ref);
		nvnc_client_close(client);
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
			nvnc_log(NVNC_LOG_INFO, "Client connection error: %p (ref %d)",
				  client, client->ref);
			nvnc_client_close(client);
		}

		return;
	}

	client->buffer_len += n_read;

	while (1) {
		int rc = try_read_client_message(client);
		if (rc == 0)
			break;

		client->buffer_index += rc;

	}

	assert(client->buffer_index <= client->buffer_len);

	client->buffer_len -= client->buffer_index;
	memmove(client->msg_buffer, client->msg_buffer + client->buffer_index,
	        client->buffer_len);
	client->buffer_index = 0;
}

static void record_peer_hostname(int fd, struct nvnc_client* client)
{
	struct sockaddr_storage storage;
	struct sockaddr* peer = (struct sockaddr*)&storage;
	socklen_t peerlen = sizeof(storage);
	if (getpeername(fd, peer, &peerlen) == 0) {
		if (peer->sa_family == AF_UNIX) {
			snprintf(client->hostname, sizeof(client->hostname),
					"unix domain socket");
		} else {
			getnameinfo(peer, peerlen,
					client->hostname, sizeof(client->hostname),
					NULL, 0, // no need for port
					0);
		}
	}
}

static void on_connection(void* obj)
{
	struct nvnc* server = aml_get_userdata(obj);

	struct nvnc_client* client = calloc(1, sizeof(*client));
	if (!client)
		return;

	client->ref = 1;
	client->server = server;
	client->quality = 10; /* default to lossless */

	int fd = accept(server->fd, NULL, 0);
	if (fd < 0) {
		nvnc_log(NVNC_LOG_WARNING, "Failed to accept a connection");
		goto accept_failure;
	}

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	record_peer_hostname(fd, client);

#ifdef ENABLE_WEBSOCKET
	if (server->socket_type == NVNC__SOCKET_WEBSOCKET)
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

	stream_send(client->net_stream, payload, NULL, NULL);

	LIST_INSERT_HEAD(&server->clients, client, link);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VERSION;

	nvnc_log(NVNC_LOG_INFO, "New client connection from %s: %p (ref %d)", client->hostname, client, client->ref);

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

		int sndbuf = 65536;
		if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(int)) < 0) {
			nvnc_log(NVNC_LOG_DEBUG, "Failed to set SO_SNDBUF: %m");
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

static int bind_address(const char* name, uint16_t port,
		enum nvnc__socket_type type)
{
	switch (type) {
	case NVNC__SOCKET_TCP:
	case NVNC__SOCKET_WEBSOCKET:
		return bind_address_tcp(name, port);
	case NVNC__SOCKET_UNIX:
		return bind_address_unix(name);
	}

	nvnc_log(NVNC_LOG_PANIC, "Unknown socket address type");
	return -1;
}

static struct nvnc* open_common(const char* address, uint16_t port,
		enum nvnc__socket_type type)
{
	nvnc__log_init();

	aml_require_workers(aml_get_default(), -1);

	struct nvnc* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->socket_type = type;

	strcpy(self->name, DEFAULT_NAME);

	LIST_INIT(&self->clients);

	self->fd = bind_address(address, port, type);
	if (self->fd < 0)
		goto bind_failure;

	if (listen(self->fd, 16) < 0)
		goto listen_failure;

	self->poll_handle = aml_handler_new(self->fd, on_connection, self, NULL);
	if (!self->poll_handle)
		goto handle_failure;

	if (aml_start(aml_get_default(), self->poll_handle) < 0)
		goto poll_start_failure;

	return self;

poll_start_failure:
	aml_unref(self->poll_handle);
handle_failure:
listen_failure:
	close(self->fd);
	if (type == NVNC__SOCKET_UNIX) {
		unlink(address);
	}
bind_failure:
	free(self);

	return NULL;
}

EXPORT
struct nvnc* nvnc_open(const char* address, uint16_t port)
{
	return open_common(address, port, NVNC__SOCKET_TCP);
}

EXPORT
struct nvnc* nvnc_open_websocket(const char *address, uint16_t port)
{
#ifdef ENABLE_WEBSOCKET
	return open_common(address, port, NVNC__SOCKET_WEBSOCKET);
#else
	return NULL;
#endif
}

EXPORT
struct nvnc* nvnc_open_unix(const char* address)
{
	return open_common(address, 0, NVNC__SOCKET_UNIX);
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
void nvnc_close(struct nvnc* self)
{
	struct nvnc_client* client;

	nvnc_cleanup_fn cleanup = self->common.cleanup_fn;
	if (cleanup)
		cleanup(self->common.userdata);

	if (self->display)
		nvnc_display_unref(self->display);

	if (self->cursor.buffer)
		nvnc_fb_unref(self->cursor.buffer);

	struct nvnc_client* tmp;
	LIST_FOREACH_SAFE (client, &self->clients, link, tmp)
		client_unref(client);

	aml_stop(aml_get_default(), self->poll_handle);
	unlink_fd_path(self->fd);
	close(self->fd);

#ifdef ENABLE_TLS
	if (self->tls_creds) {
		gnutls_certificate_free_credentials(self->tls_creds);
		gnutls_global_deinit();
	}
#endif

	aml_unref(self->poll_handle);
	free(self);
}

static void complete_fb_update(struct nvnc_client* client)
{
	client->is_updating = false;
	assert(client->current_fb);
	nvnc_fb_release(client->current_fb);
	nvnc_fb_unref(client->current_fb);
	client->current_fb = NULL;
	process_fb_update_requests(client);
	client_unref(client);
	DTRACE_PROBE2(neatvnc, update_fb_done, client, pts);
}

static void on_write_frame_done(void* userdata, enum stream_req_status status)
{
	struct nvnc_client* client = userdata;
	complete_fb_update(client);
}

static enum rfb_encodings choose_frame_encoding(struct nvnc_client* client,
		struct nvnc_fb* fb)
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

static void finish_fb_update(struct nvnc_client* client, struct rcbuf* payload,
		int n_rects, uint64_t pts)
{
	client_ref(client);

	if (client->net_stream->state == STREAM_STATE_CLOSED)
		goto complete;

	DTRACE_PROBE2(neatvnc, send_fb_start, client, pts);
	n_rects += will_send_pts(client, pts) ? 1 : 0;
	struct rfb_server_fb_update_msg update_msg = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(n_rects),
	};
	if (stream_write(client->net_stream, &update_msg,
			sizeof(update_msg), NULL, NULL) < 0)
		goto complete;

	if (send_pts_rect(client, pts) < 0)
		goto complete;

	rcbuf_ref(payload);
	if (stream_send(client->net_stream, payload,
				on_write_frame_done, client) < 0)
		goto complete;

	DTRACE_PROBE2(neatvnc, send_fb_done, client, pts);
	return;

complete:
	complete_fb_update(client);
}

static void on_encode_frame_done(struct encoder* encoder, struct rcbuf* result,
		uint64_t pts)
{
	struct nvnc_client* client = encoder->userdata;
	client->encoder->on_done = NULL;
	client->encoder->userdata = NULL;
	finish_fb_update(client, result, encoder->n_rects, pts);
	client_unref(client);
}

static int send_desktop_resize(struct nvnc_client* client, struct nvnc_fb* fb)
{
	if (!client_has_encoding(client, RFB_ENCODING_DESKTOPSIZE) &&
	    !client_has_encoding(client, RFB_ENCODING_EXTENDEDDESKTOPSIZE)) {
		nvnc_log(NVNC_LOG_ERROR, "Client does not support desktop resizing. Closing connection...");
		nvnc_client_close(client);
		return -1;
	}

	client->known_width = fb->width;
	client->known_height = fb->height;

	if (client->encoder)
		encoder_resize(client->encoder, fb->width, fb->height);

	pixman_region_union_rect(&client->damage, &client->damage, 0, 0,
			fb->width, fb->height);

	if (client_has_encoding(client, RFB_ENCODING_EXTENDEDDESKTOPSIZE)) {
		send_extended_desktop_size(client,
				RFB_RESIZE_INITIATOR_SERVER,
				RFB_RESIZE_STATUS_SUCCESS);
		return 0;
	}

	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_DESKTOPSIZE),
		.width = htons(fb->width),
		.height = htons(fb->height),
	};

	stream_write(client->net_stream, &head, sizeof(head), NULL, NULL);
	stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	return 0;
}

static int send_qemu_key_ext_frame(struct nvnc_client* client)
{
	struct rfb_server_fb_update_msg head = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(1),
	};

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_QEMU_EXT_KEY_EVENT),
	};

	stream_write(client->net_stream, &head, sizeof(head), NULL, NULL);
	stream_write(client->net_stream, &rect, sizeof(rect), NULL, NULL);
	return 0;
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
const char* nvnc_client_get_hostname(const struct nvnc_client* client) {
	if (client->hostname[0] == '\0')
		return NULL;
	return client->hostname;
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
	stream_close(client->net_stream);
	client_unref(client);
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

EXPORT
void nvnc_set_name(struct nvnc* self, const char* name)
{
	strncpy(self->name, name, sizeof(self->name));
	self->name[sizeof(self->name) - 1] = '\0';
}

EXPORT
bool nvnc_has_auth(void)
{
#ifdef ENABLE_TLS
	return true;
#else
	return false;
#endif
}

EXPORT
int nvnc_enable_auth(struct nvnc* self, const char* privkey_path,
                     const char* cert_path, nvnc_auth_fn auth_fn,
                     void* userdata)
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

	self->auth_fn = auth_fn;
	self->auth_ud = userdata;

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
void nvnc_set_cursor(struct nvnc* self, struct nvnc_fb* fb, uint16_t width,
		uint16_t height, uint16_t hotspot_x, uint16_t hotspot_y,
		bool is_damaged)
{
	if (self->cursor.buffer) {
		nvnc_fb_release(self->cursor.buffer);
		nvnc_fb_unref(self->cursor.buffer);
	}

	self->cursor.buffer = fb;

	if (fb) {
		// TODO: Hash cursors to check if they actually changed?
		nvnc_fb_ref(fb);
		nvnc_fb_hold(fb);

		self->cursor.width = width;
		self->cursor.height = height;
		self->cursor.hotspot_x = hotspot_x;
		self->cursor.hotspot_y = hotspot_y;

	} else {
		self->cursor.width = width;
		self->cursor.height = height;
		self->cursor.hotspot_x = 0;
		self->cursor.hotspot_y = 0;
	}

	if (!is_damaged)
		return;

	self->cursor_seq++;

	struct nvnc_client* client;
	LIST_FOREACH(client, &self->clients, link)
		process_fb_update_requests(client);
}
