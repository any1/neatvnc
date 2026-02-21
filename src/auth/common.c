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

static int security_send_failure_reason(struct nvnc_client* client,
		uint32_t result_code, const char* reason_string)
{
	char buffer[256];

	uint32_t* result = (uint32_t*)buffer;

	struct rfb_error_reason* reason =
	        (struct rfb_error_reason*)(buffer + sizeof(*result));

	*result = htonl(result_code);
	reason->length = htonl(strlen(reason_string));
	strcpy(reason->message, reason_string);

	size_t len = sizeof(*result) + sizeof(*reason) + strlen(reason_string);
	stream_write(client->net_stream, buffer, len, close_after_write,
			client->net_stream);

	stream_ref(client->net_stream);

	nvnc_client_close(client);
	return 0;
}

int security_handshake_failed(struct nvnc_client* client, const char* username,
		const char* reason_string)
{
	if (username)
		nvnc_log(NVNC_LOG_INFO, "Security handshake failed for \"%s\": %s",
				username, reason_string);
	else
		nvnc_log(NVNC_LOG_INFO, "Security handshake failed: %s",
				reason_string);

	return security_send_failure_reason(client, RFB_SECURITY_HANDSHAKE_FAILED,
			reason_string);
}

int security_type_invalid(struct nvnc_client* client,
		const char* reason_string)
{
	nvnc_log(NVNC_LOG_WARNING, "Connection rejected: %s", reason_string);

	return security_send_failure_reason(client, RFB_SECURITY_TYPE_INVALID,
			reason_string);
}

int security_handshake_ok(struct nvnc_client* client, const char* username)
{
	if (username) {
		nvnc_log(NVNC_LOG_INFO, "User \"%s\" authenticated", username);

		strncpy(client->username, username, sizeof(client->username));
		client->username[sizeof(client->username) - 1] = '\0';
	}

	uint32_t result = htonl(RFB_SECURITY_HANDSHAKE_OK);
	return stream_write(client->net_stream, &result, sizeof(result), NULL,
			NULL);
}
