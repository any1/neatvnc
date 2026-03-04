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

#include <string.h>
#include <nettle/des.h>

#define DES_CHALLENGE_SIZE 16

/* VNC uses DES with reversed bit order within each byte of the key. */
static void des_vnc_key_reverse_bits(uint8_t* dst, const char* src)
{
	for (int i = 0; i < 8; i++) {
		uint8_t b = (uint8_t)src[i];
		b = ((b & 0xf0) >> 4) | ((b & 0x0f) << 4);
		b = ((b & 0xcc) >> 2) | ((b & 0x33) << 2);
		b = ((b & 0xaa) >> 1) | ((b & 0x55) << 1);
		dst[i] = b;
	}
}

static void des_vnc_encrypt(uint8_t* dst, const uint8_t* src,
		const char* password)
{
	char key[8] = {};
	size_t len = strlen(password);
	if (len > 8)
		len = 8;
	memcpy(key, password, len);

	uint8_t vnc_key[8];
	des_vnc_key_reverse_bits(vnc_key, key);

	struct des_ctx ctx;
	des_set_key(&ctx, vnc_key);

	/* DES encrypts 8 bytes at a time; the challenge is 16 bytes. */
	des_encrypt(&ctx, 8, dst, src);
	des_encrypt(&ctx, 8, dst + 8, src + 8);
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

	uint8_t expected[DES_CHALLENGE_SIZE];
	des_vnc_encrypt(expected, client->des_challenge, server->des_password);

	update_min_rtt(client);

	if (memcmp(response, expected, DES_CHALLENGE_SIZE) != 0) {
		security_handshake_failed(client, NULL,
				"Invalid password");
		return -1;
	}

	security_handshake_ok(client, NULL);
	client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	return DES_CHALLENGE_SIZE;
}
