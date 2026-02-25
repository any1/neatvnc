/*
 * Copyright (c) 2026 Andrian Budantsov
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
#include "auth/des-auth.h"
#include "crypto.h"
#include "des-rfb.h"

#include <string.h>

#define DES_CHALLENGE_SIZE 16

bool des_auth_verify(const uint8_t* challenge, const uint8_t* response,
		const char* password)
{
	uint8_t expected[DES_CHALLENGE_SIZE];
	des_vnc_encrypt(expected, challenge, password);
	return memcmp(expected, response, DES_CHALLENGE_SIZE) == 0;
}

int des_auth_send_challenge(struct nvnc_client* client)
{
	crypto_random(client->des_challenge, DES_CHALLENGE_SIZE);
	return stream_write(client->net_stream, client->des_challenge,
			DES_CHALLENGE_SIZE, NULL, NULL);
}

int des_auth_handle_response(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (client->buffer_len - client->buffer_index < DES_CHALLENGE_SIZE)
		return 0;

	uint8_t* response = client->msg_buffer + client->buffer_index;

	update_min_rtt(client);

	struct nvnc_auth_creds creds = {
		.type = NVNC_AUTH_CREDS_DES,
		.username = NULL,
		.des = {
			.challenge = client->des_challenge,
			.response = response,
		},
	};

	if (!server->auth_fn(&creds, server->auth_ud)) {
		security_handshake_failed(client, NULL,
				"Invalid password");
		return -1;
	}

	security_handshake_ok(client, NULL);
	client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	return DES_CHALLENGE_SIZE;
}
