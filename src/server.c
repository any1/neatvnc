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

#include "rfb-proto.h"
#include "zrle.h"
#include "tight.h"
#include "raw-encoding.h"
#include "vec.h"
#include "type-macros.h"
#include "fb.h"
#include "display.h"
#include "neatvnc.h"
#include "common.h"
#include "pixels.h"
#include "stream.h"
#include "config.h"
#include "logging.h"
#include "usdt.h"

#include <stdlib.h>
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
#include <netdb.h>

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

struct fb_update_work {
	struct aml_work* work;
	struct nvnc_client* client;
	struct pixman_region16 region;
	struct rfb_pixel_format server_fmt;
	struct vec frame;
	struct nvnc_fb* fb;
};

int schedule_client_update_fb(struct nvnc_client* client);

#if defined(GIT_VERSION)
EXPORT const char nvnc_version[] = GIT_VERSION;
#elif defined(PROJECT_VERSION)
EXPORT const char nvnc_version[] = PROJECT_VERSION;
#else
EXPORT const char nvnc_version[] = "UNKNOWN";
#endif

static void client_close(struct nvnc_client* client)
{
	log_debug("client_close(%p): ref %d\n", client, client->ref);

	nvnc_client_fn fn = client->cleanup_fn;
	if (fn)
		fn(client);

	LIST_REMOVE(client, link);
	stream_destroy(client->net_stream);
	tight_encoder_destroy(&client->tight_encoder);
	deflateEnd(&client->z_stream);
	pixman_region_fini(&client->damage);
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
	log_debug("close_after_write(%p): ref %d\n", client, client->ref);
	stream_close(client->net_stream);
	client_unref(client);
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
		stream_close(client->net_stream);
		client_unref(client);
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

	if (server->auth_fn(username, password, server->auth_ud)) {
		log_debug("User \"%s\" authenticated\n", username);
		security_handshake_ok(client);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		log_debug("User \"%s\" rejected\n", username);
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
			log_debug("disconnect other client %p (ref %d)\n",
			          node, node->ref);
			stream_close(node->net_stream);
			client_unref(node);
		}

}

static void send_server_init_message(struct nvnc_client* client)
{
	struct nvnc* server = client->server;
	struct nvnc_display* display = server->display;

	size_t name_len = strlen(server->name);
	size_t size = sizeof(struct rfb_server_init_msg) + name_len;

	if (!display) {
		log_debug("Tried to send init message, but no display has been added\n");
		goto close;
	}

	if (!display->buffer) {
		log_debug("Tried to send init message, but no framebuffers have been set\n");
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
	stream_close(client->net_stream);
	client_unref(client);
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
		stream_close(client->net_stream);
		client_unref(client);
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
		case RFB_ENCODING_CURSOR:
		case RFB_ENCODING_DESKTOPSIZE:
		case RFB_ENCODING_JPEG_HIGHQ:
		case RFB_ENCODING_JPEG_LOWQ:
			client->encodings[n++] = encoding;
		}
	}

	client->n_encodings = n;

	return sizeof(*msg) + 4 * n_encodings;
}

static void process_fb_update_requests(struct nvnc_client* client)
{
	if (!client->server->display || !client->server->display->buffer)
		return;

	if (client->net_stream->state == STREAM_STATE_CLOSED)
		return;

	if (!pixman_region_not_empty(&client->damage))
		return;

	if (client->is_updating || client->n_pending_requests == 0)
		return;

	client->is_updating = true;
	if (schedule_client_update_fb(client) < 0)
		client->is_updating = false;
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
	if (!incremental)
		pixman_region_union_rect(&client->damage, &client->damage, x, y,
		                         width, height);

	DTRACE_PROBE1(neatvnc, update_fb_request, client);

	nvnc_fb_req_fn fn = server->fb_req_fn;
	if (fn)
		fn(client, incremental, x, y, width, height);

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
	struct nvnc* server = client->server;
	struct rfb_cut_text_msg* msg =
	        (struct rfb_cut_text_msg*)(client->msg_buffer +
	                                   client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint32_t length = ntohl(msg->length);

	/* Messages greater than this size are unsupported */
	if (length > MSG_BUFFER_SIZE - sizeof(*msg)) {
		stream_close(client->net_stream);
		client_unref(client);
		return 0;
	}

	if (client->buffer_len - client->buffer_index < sizeof(*msg) + length)
		return 0;

	nvnc_cut_text_fn fn = server->cut_text_fn;
	if (fn) {
		fn(server, msg->text, length);
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
	}

	log_debug("Got uninterpretable message from client: %p (ref %d)\n",
	          client, client->ref);
	stream_close(client->net_stream);
	client_unref(client);
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

	abort();
	return 0;
}

static void on_client_event(struct stream* stream, enum stream_event event)
{
	struct nvnc_client* client = stream->userdata;

	assert(client->net_stream && client->net_stream == stream);

	if (event == STREAM_EVENT_REMOTE_CLOSED) {
		log_debug("Client %p (%d) hung up\n", client, client->ref);
		stream_close(stream);
		client_unref(client);
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
			log_debug("Client connection error: %p (ref %d)\n",
				  client, client->ref);
			stream_close(stream);
			client_unref(client);
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

static void on_connection(void* obj)
{
	struct nvnc* server = aml_get_userdata(obj);

	struct nvnc_client* client = calloc(1, sizeof(*client));
	if (!client)
		return;

	client->ref = 1;
	client->server = server;

	int fd = accept(server->fd, NULL, 0);
	if (fd < 0) {
		log_debug("Failed to accept a connection\n");
		goto accept_failure;
	}

	client->net_stream = stream_new(fd, on_client_event, client);
	if (!client->net_stream) {
		log_debug("OOM\n");
		goto stream_failure;
	}

	int rc = deflateInit2(&client->z_stream,
	                      /* compression level: */ 1,
	                      /*            method: */ Z_DEFLATED,
	                      /*       window bits: */ 15,
	                      /*         mem level: */ 9,
	                      /*          strategy: */ Z_DEFAULT_STRATEGY);

	if (rc != Z_OK) {
		log_debug("OOM\n");
		goto deflate_failure;
	}

	if (!server->display->buffer) {
		log_debug("No display buffer has been set\n");
		goto buffer_failure;
	}

	int width = server->display->buffer->width;
	int height = server->display->buffer->height;
	if (tight_encoder_init(&client->tight_encoder, width, height) < 0) {
		log_debug("OOM\n");
		goto tight_failure;
	}

	pixman_region_init(&client->damage);

	struct rcbuf* payload = rcbuf_from_string(RFB_VERSION_MESSAGE);
	if (!payload) {
		log_debug("OOM\n");
		goto payload_failure;
	}

	stream_send(client->net_stream, payload, NULL, NULL);

	LIST_INSERT_HEAD(&server->clients, client, link);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VERSION;

	log_debug("New client connection: %p (ref %d)\n", client, client->ref);

	return;

payload_failure:
	tight_encoder_destroy(&client->tight_encoder);
	pixman_region_fini(&client->damage);
tight_failure:
buffer_failure:
	deflateEnd(&client->z_stream);
deflate_failure:
	stream_destroy(client->net_stream);
stream_failure:
	close(fd);
accept_failure:
	free(client);
}

static int bind_address(const char* name, int port)
{
	struct addrinfo hints = {
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE,
	};

	struct addrinfo* result;

	char service[256];
	snprintf(service, sizeof(service), "%d", port);

	int rc = getaddrinfo(name, service, &hints, &result);
	if (rc != 0)
		return -1;

	int fd = -1;

	for (struct addrinfo* p = result; p != NULL; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0)
			continue;

		int one = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0)
			goto failure;

		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0)
			break;

failure:
		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);
	return fd;
}

static bool nvnc__is_damaged(struct nvnc* self)
{
	struct nvnc_client* client;
	LIST_FOREACH(client, &self->clients, link)
		if (pixman_region_not_empty(&client->damage))
			return true;

	return false;
}

static void on_main_dispatch(void* aml_obj)
{
	struct nvnc* self = aml_get_userdata(aml_obj);
	struct nvnc_client* client;

	if (!nvnc__is_damaged(self))
		return;

	LIST_FOREACH(client, &self->clients, link)
		if (client->is_updating) {
			log_debug("Can't render yet: still encoding for client %p\n",
			          client);
			return;
		}

	struct nvnc_display* display = self->display;
	if (display && display->render_fn)
		display->render_fn(display, display->buffer);

	LIST_FOREACH(client, &self->clients, link)
		process_fb_update_requests(client);
}

EXPORT
struct nvnc* nvnc_open(const char* address, uint16_t port)
{
	aml_require_workers(aml_get_default(), -1);

	struct nvnc* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	strcpy(self->name, DEFAULT_NAME);

	LIST_INIT(&self->clients);

	self->fd = bind_address(address, port);
	if (self->fd < 0)
		goto failure;

	if (listen(self->fd, 16) < 0)
		goto failure;

	self->poll_handle = aml_handler_new(self->fd, on_connection, self, NULL);
	if (!self->poll_handle)
		goto failure;

	if (aml_start(aml_get_default(), self->poll_handle) < 0)
		goto poll_start_failure;

	self->dispatch_handler = aml_idle_new(on_main_dispatch, self, NULL);
	if (!self->dispatch_handler)
		goto new_idle_failure;

	if (aml_start(aml_get_default(), self->dispatch_handler) < 0)
		goto idle_start_failure;

	return self;

idle_start_failure:
	aml_unref(self->dispatch_handler);
new_idle_failure:
	aml_stop(aml_get_default(), self->poll_handle);
poll_start_failure:
	aml_unref(self->poll_handle);
failure:
	close(self->fd);
	return NULL;
}

EXPORT
void nvnc_close(struct nvnc* self)
{
	struct nvnc_client* client;

	if (self->display)
		nvnc_display_unref(self->display);

	struct nvnc_client* tmp;
	LIST_FOREACH_SAFE (client, &self->clients, link, tmp)
		client_unref(client);

	aml_stop(aml_get_default(), self->dispatch_handler);
	aml_stop(aml_get_default(), self->poll_handle);
	close(self->fd);

#ifdef ENABLE_TLS
	if (self->tls_creds) {
		gnutls_certificate_free_credentials(self->tls_creds);
		gnutls_global_deinit();
	}
#endif

	aml_unref(self->dispatch_handler);
	aml_unref(self->poll_handle);
	free(self);
}

static void on_write_frame_done(void* userdata, enum stream_req_status status)
{
	struct nvnc_client* client = userdata;
	client->is_updating = false;
	client_unref(client);
}

static enum rfb_encodings choose_frame_encoding(struct nvnc_client* client)
{
	for (size_t i = 0; i < client->n_encodings; ++i)
		switch (client->encodings[i]) {
		case RFB_ENCODING_RAW:
		case RFB_ENCODING_TIGHT:
		case RFB_ENCODING_ZRLE:
			return client->encodings[i];
		default:
			break;
		}

	return RFB_ENCODING_RAW;
}

static enum tight_quality client_get_tight_quality(struct nvnc_client* client)
{
	if (client->pixfmt.bits_per_pixel != 16 &&
	    client->pixfmt.bits_per_pixel != 32)
		return TIGHT_QUALITY_LOSSLESS;

	for (size_t i = 0; i < client->n_encodings; ++i)
		switch (client->encodings[i]) {
		case RFB_ENCODING_JPEG_HIGHQ: return TIGHT_QUALITY_HIGH;
		case RFB_ENCODING_JPEG_LOWQ: return TIGHT_QUALITY_LOW;
		default:;
		}

	return TIGHT_QUALITY_LOSSLESS;
}

static bool client_has_encoding(const struct nvnc_client* client,
		enum rfb_encodings encoding)
{
	for (size_t i = 0; i < client->n_encodings; ++i)
		if (client->encodings[i] == encoding)
			return true;

	return false;
}

static void do_client_update_fb(void* work)
{
	struct fb_update_work* update = aml_get_userdata(work);
	struct nvnc_client* client = update->client;
	struct nvnc_fb* fb = update->fb;

	enum rfb_encodings encoding = choose_frame_encoding(client);

	if (!client->has_pixfmt) {
		rfb_pixfmt_from_fourcc(&client->pixfmt, fb->fourcc_format);
		client->has_pixfmt = true;
	}

	switch (encoding) {
	case RFB_ENCODING_RAW:
		raw_encode_frame(&update->frame, &client->pixfmt, fb,
		                 &update->server_fmt, &update->region);
		break;
	case RFB_ENCODING_TIGHT:;
		enum tight_quality quality = client_get_tight_quality(client);
		tight_encode_frame(&client->tight_encoder, &update->frame,
				&client->pixfmt, fb, &update->server_fmt,
				&update->region, quality);
		break;
	case RFB_ENCODING_ZRLE:
		zrle_encode_frame(&client->z_stream, &update->frame,
		                  &client->pixfmt, fb, &update->server_fmt,
		                  &update->region);
		break;
	default:
		break;
	}
}

static void on_client_update_fb_done(void* work)
{
	struct fb_update_work* update = aml_get_userdata(work);
	struct nvnc_client* client = update->client;
	struct vec* frame = &update->frame;

	nvnc_fb_unref(update->fb);

	client_ref(client);

	if (client->net_stream->state != STREAM_STATE_CLOSED) {
		struct rcbuf* payload = rcbuf_new(frame->data, frame->len);
		DTRACE_PROBE1(neatvnc, send_fb_start, client);
		stream_send(client->net_stream, payload, on_write_frame_done,
		            client);
		DTRACE_PROBE1(neatvnc, send_fb_done, client);
	} else {
		client->is_updating = false;
		vec_destroy(frame);
		client_unref(client);
	}

	client->n_pending_requests--;

	DTRACE_PROBE1(neatvnc, update_fb_done, client);

	pixman_region_fini(&update->region);

	client_unref(client);
}

static int send_desktop_resize(struct nvnc_client* client, struct nvnc_fb* fb)
{
	if (!client_has_encoding(client, RFB_ENCODING_DESKTOPSIZE)) {
		log_error("Client does not support desktop resizing. Closing connection...\n");
		stream_close(client->net_stream);
		client_unref(client);
		return -1;
	}

	client->known_width = fb->width;
	client->known_height = fb->height;

	tight_encoder_resize(&client->tight_encoder, fb->width, fb->height);

	pixman_region_union_rect(&client->damage, &client->damage, 0, 0,
			fb->width, fb->height);

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

int schedule_client_update_fb(struct nvnc_client* client)
{
	struct nvnc_fb* fb = client->server->display->buffer;
	assert(fb);

	DTRACE_PROBE1(neatvnc, update_fb_start, client);

	struct fb_update_work* work = calloc(1, sizeof(*work));
	if (!work)
		return -1;

	if (rfb_pixfmt_from_fourcc(&work->server_fmt, fb->fourcc_format) < 0)
		goto pixfmt_failure;

	work->client = client;
	work->fb = fb;

	if (fb->width != client->known_width
	    || fb->height != client->known_height)
		if (send_desktop_resize(client, fb) < 0)
			goto resize_failure;

	/* The client's damage is exchanged for an empty one */
	work->region = client->damage;
	pixman_region_init(&client->damage);

	int rc = vec_init(&work->frame, fb->width * fb->height * 3 / 2);
	if (rc < 0)
		goto vec_failure;

	client_ref(client);
	nvnc_fb_ref(fb);

	struct aml_work* obj =
		aml_work_new(do_client_update_fb, on_client_update_fb_done,
		             work, free);
	if (!obj) {
		goto oom_failure;
	}

	rc = aml_start(aml_get_default(), obj);
	aml_unref(obj);
	if (rc < 0)
		goto start_failure;

	work->work = obj;

	return 0;

start_failure:
	work = NULL; /* handled in unref */
oom_failure:
	nvnc_fb_unref(fb);
	client_unref(client);
	vec_destroy(&work->frame);
vec_failure:
pixfmt_failure:
resize_failure:
	free(work);
	return -1;
}

void nvnc__damage_region(struct nvnc* self, const struct pixman_region16* damage)
{
	struct nvnc_client* client;

	LIST_FOREACH(client, &self->clients, link)
		if (client->net_stream->state != STREAM_STATE_CLOSED)
			pixman_region_union(&client->damage, &client->damage,
					    (struct pixman_region16*)damage);
}

EXPORT
void nvnc_set_userdata(void* self, void* userdata)
{
	struct nvnc_common* common = self;
	common->userdata = userdata;
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
void nvnc_set_cut_text_receive_fn(struct nvnc* self, nvnc_cut_text_fn fn)
{
	self->cut_text_fn = fn;
}

EXPORT
void nvnc_add_display(struct nvnc* self, struct nvnc_display* display)
{
	if (self->display) {
		log_error("Multiple displays are not implemented. Aborting!\n");
		abort();
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
		log_error("GnuTLS: Failed to initialise: %s\n",
		          gnutls_strerror(rc));
		return -1;
	}

	rc = gnutls_certificate_allocate_credentials(&self->tls_creds);
	if (rc != GNUTLS_E_SUCCESS) {
		log_error("GnuTLS: Failed to allocate credentials: %s\n",
		          gnutls_strerror(rc));
		goto cert_alloc_failure;
	}

	rc = gnutls_certificate_set_x509_key_file(
		self->tls_creds, cert_path, privkey_path, GNUTLS_X509_FMT_PEM);
	if (rc != GNUTLS_E_SUCCESS) {
		log_error("GnuTLS: Failed to load credentials: %s\n",
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
