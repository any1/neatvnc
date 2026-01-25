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
#include "auth/vnc-auth.h"
#include "crypto.h"
#include "logging.h"

#include <string.h>
#include <nettle/des.h>

static void des_vnc_key_reverse_bits(uint8_t* key)
{
	for (int i = 0; i < 8; i++) {
		uint8_t b = key[i];
		b = ((b & 0xf0) >> 4) | ((b & 0x0f) << 4);
		b = ((b & 0xcc) >> 2) | ((b & 0x33) << 2);
		b = ((b & 0xaa) >> 1) | ((b & 0x55) << 1);
		key[i] = b;
	}
}

static void des_vnc_encrypt(uint8_t* response, const uint8_t* challenge,
		const char* password)
{
	uint8_t key[8] = {0};
	size_t passlen = strlen(password);
	if (passlen > 8)
		passlen = 8;
	memcpy(key, password, passlen);

	des_vnc_key_reverse_bits(key);

	struct des_ctx ctx;
	des_set_key(&ctx, key);
	des_encrypt(&ctx, 8, response, challenge);
	des_encrypt(&ctx, 8, response + 8, challenge + 8);
}

int vnc_auth_send_challenge(struct nvnc_client* client)
{
	crypto_random(client->des_vnc_auth_challenge, 16);
	stream_write(client->net_stream, client->des_vnc_auth_challenge, 16,
			NULL, NULL);
	return 0;
}

int vnc_auth_handle_response(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (client->buffer_len - client->buffer_index < 16)
		return 0;

	uint8_t* response = client->msg_buffer + client->buffer_index;
	uint8_t expected[16];
	des_vnc_encrypt(expected, client->des_vnc_auth_challenge,
			server->des_vnc_auth_password);

	update_min_rtt(client);

	if (memcmp(response, expected, 16) == 0) {
		nvnc_log(NVNC_LOG_INFO, "VNC Auth succeeded");
		security_handshake_ok(client, NULL);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		nvnc_log(NVNC_LOG_WARNING, "VNC Auth failed");
		security_handshake_failed(client, NULL, "Authentication failed");
		return -1;
	}

	return 16;
}
