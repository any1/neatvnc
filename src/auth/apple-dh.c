/*
 * Copyright (c) 2023 - 2024 Andri Yngvason
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
#include "auth/apple-dh.h"

#define APPLE_DH_SERVER_KEY_LENGTH 256

int apple_dh_send_public_key(struct nvnc_client* client)
{
	client->apple_dh_secret = crypto_keygen();
	assert(client->apple_dh_secret);

	struct crypto_key* pub =
		crypto_derive_public_key(client->apple_dh_secret);
	assert(pub);

	uint8_t mod[APPLE_DH_SERVER_KEY_LENGTH] = {};
	int mod_len = crypto_key_p(pub, mod, sizeof(mod));
	assert(mod_len == sizeof(mod));

	uint8_t q[APPLE_DH_SERVER_KEY_LENGTH] = {};
	int q_len = crypto_key_q(pub, q, sizeof(q));
	assert(q_len == sizeof(q));

	struct rfb_apple_dh_server_msg msg = {
		.generator = htons(crypto_key_g(client->apple_dh_secret)),
		.key_size = htons(q_len),
	};

	stream_write(client->net_stream, &msg, sizeof(msg), NULL, NULL);
	stream_write(client->net_stream, mod, mod_len, NULL, NULL);
	stream_write(client->net_stream, q, q_len, NULL, NULL);

	crypto_key_del(pub);
	return 0;
}

int apple_dh_handle_response(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	struct rfb_apple_dh_client_msg* msg =
	        (void*)(client->msg_buffer + client->buffer_index);

	uint8_t p[APPLE_DH_SERVER_KEY_LENGTH];
	int key_len = crypto_key_p(client->apple_dh_secret, p, sizeof(p));
	assert(key_len == sizeof(p));

	if (client->buffer_len - client->buffer_index < sizeof(*msg) + key_len)
		return 0;

	int g = crypto_key_g(client->apple_dh_secret);

	struct crypto_key* remote_key = crypto_key_new(g, p, key_len,
			msg->public_key, key_len);
	assert(remote_key);

	struct crypto_key* shared_secret =
		crypto_derive_shared_secret(client->apple_dh_secret, remote_key);
	assert(shared_secret);

	uint8_t shared_buf[APPLE_DH_SERVER_KEY_LENGTH];
	crypto_key_q(shared_secret, shared_buf, sizeof(shared_buf));
	crypto_key_del(shared_secret);

	uint8_t hash[16] = {};
	crypto_hash_one(hash, sizeof(hash), CRYPTO_HASH_MD5, shared_buf,
			sizeof(shared_buf));

	struct crypto_cipher* cipher;
	cipher = crypto_cipher_new(NULL, hash, CRYPTO_CIPHER_AES128_ECB);
	assert(cipher);

	char username[128] = {};
	char* password = username + 64;

	crypto_cipher_decrypt(cipher, (uint8_t*)username, NULL,
			msg->encrypted_credentials, sizeof(username), NULL, 0);
	username[63] = '\0';
	username[127] = '\0';
	crypto_cipher_del(cipher);

	update_min_rtt(client);

	if (server->auth_fn(username, password, server->auth_ud)) {
		security_handshake_ok(client, username);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		security_handshake_failed(client, username,
				"Invalid username or password");
	}

	return sizeof(*msg) + key_len;
}
