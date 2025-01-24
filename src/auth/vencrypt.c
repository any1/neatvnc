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

#include "common.h"
#include "stream/stream.h"
#include "auth/auth.h"
#include "auth/vencrypt.h"

#include <sys/param.h>

static int send_byte(struct nvnc_client* client, uint8_t value)
{
	return stream_write(client->net_stream, &value, 1, NULL, NULL);
}

static int send_byte_and_close(struct nvnc_client* client, uint8_t value)
{
	return stream_write(client->net_stream, &value, 1, close_after_write,
			client);
}

int vencrypt_send_version(struct nvnc_client* client)
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
		security_handshake_failed(client, NULL,
				"Unsupported VeNCrypt version");
		return sizeof(*msg);
	}

	send_byte(client, 0);

	struct rfb_vencrypt_subtypes_msg result = { .n = 1, };
	result.types[0] = htonl(RFB_VENCRYPT_X509_PLAIN);

	update_min_rtt(client);

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

	update_min_rtt(client);

	send_byte(client, 1);

	if (stream_upgrade_to_tls(client->net_stream, client->server->tls_creds) < 0) {
		client->state = VNC_CLIENT_STATE_ERROR;
		nvnc_client_close(client);
		return -1;
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

	update_min_rtt(client);

	if (server->auth_fn(username, password, server->auth_ud)) {
		security_handshake_ok(client, username);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		security_handshake_failed(client, username,
				"Invalid username or password");
	}

	return sizeof(*msg) + ulen + plen;
}

int vencrypt_handle_message(struct nvnc_client* client)
{
	switch (client->state) {
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_VERSION:
		return on_vencrypt_version_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_SUBTYPE:
		return on_vencrypt_subtype_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_VENCRYPT_PLAIN_AUTH:
		return on_vencrypt_plain_auth_message(client);
	default:;
	}
	nvnc_log(NVNC_LOG_PANIC, "Unhandled client state: %d", client->state);
	return 0;
}
