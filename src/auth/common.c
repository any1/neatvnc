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

#include "auth/auth.h"
#include "stream/stream.h"
#include "rfb-proto.h"
#include "common.h"
#include "neatvnc.h"

#include <string.h>

int security_handshake_failed(struct nvnc_client* client,
		const char* reason_string)
{
	if (client->username[0])
		nvnc_log(NVNC_LOG_INFO, "Security handshake failed for \"%s\": %s",
				client->username, reason_string);
	else
		nvnc_log(NVNC_LOG_INFO, "Security handshake failed: %s",
				reason_string);

	char buffer[256];

	uint32_t* result = (uint32_t*)buffer;
	*result = htonl(RFB_SECURITY_HANDSHAKE_FAILED);

	size_t len;
	if (client->rfb_minor_version >= 8) {
		struct rfb_error_reason* reason =
			(struct rfb_error_reason*)(buffer + sizeof(*result));
		reason->length = htonl(strlen(reason_string));
		strcpy(reason->message, reason_string);
		len = sizeof(*result) + sizeof(*reason) + strlen(reason_string);
	} else {
		len = sizeof(*result);
	}

	stream_write(client->net_stream, buffer, len, NULL, NULL);
	nvnc_client_close(client);

	return 0;
}

int security_handshake_ok(struct nvnc_client* client)
{
	if (client->username[0]) {
		nvnc_log(NVNC_LOG_INFO, "User \"%s\" authenticated",
				client->username);
	}

	uint32_t result = htonl(RFB_SECURITY_HANDSHAKE_OK);
	return stream_write(client->net_stream, &result, sizeof(result), NULL,
			NULL);
}

static struct nvnc_auth_future *nvnc_auth_future_create(
		struct nvnc_client* client)
{
	struct nvnc_auth_future* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;
	weakref_observer_init(&self->client, &client->weakref);

	return self;
}

void security_handshake_authenticate(struct nvnc_client* client,
		const struct nvnc_auth_creds* creds)
{
	struct nvnc* server = client->server;

	memset(client->username, 0, sizeof(client->username));

	if (creds->username)
		strncpy(client->username, creds->username,
				sizeof(client->username) - 1);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_AUTH;

	struct nvnc_auth_future* future = nvnc_auth_future_create(client);
	if (!future) {
		security_handshake_failed(client, "Out of memory");
		return;
	}

	server->auth_fn(future, creds, server->auth_ud);

	nvnc_auth_future_unref(future);
}
