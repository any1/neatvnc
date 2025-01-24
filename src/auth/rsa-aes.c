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
#include "auth/rsa-aes.h"

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

int rsa_aes_send_public_key(struct nvnc_client* client)
{
	struct nvnc* server = client->server;

	if (!server->rsa_priv) {
		assert(!server->rsa_pub);

		nvnc_log(NVNC_LOG_WARNING, "An RSA key has not been set. A new key will be generated.");

		server->rsa_priv = crypto_rsa_priv_key_new();
		server->rsa_pub = crypto_rsa_pub_key_new();

		crypto_rsa_keygen(server->rsa_pub, server->rsa_priv);
	}
	assert(server->rsa_pub && server->rsa_priv);

	size_t key_len = crypto_rsa_pub_key_length(server->rsa_pub);
	size_t buf_len = sizeof(struct rfb_rsa_aes_pub_key_msg) + key_len * 2;

	char* buffer = calloc(1, buf_len);
	assert(buffer);
	struct rfb_rsa_aes_pub_key_msg* msg =
		(struct rfb_rsa_aes_pub_key_msg*)buffer;

	uint8_t* modulus = msg->modulus_and_exponent;
	uint8_t* exponent = msg->modulus_and_exponent + key_len;

	msg->length = htonl(key_len * 8);
	crypto_rsa_pub_key_modulus(server->rsa_pub, modulus, key_len);
	crypto_rsa_pub_key_exponent(server->rsa_pub, exponent, key_len);

	stream_send(client->net_stream, rcbuf_new(buffer, buf_len), NULL, NULL);
	return 0;
}

static int rsa_aes_send_challenge(struct nvnc_client* client,
		struct crypto_rsa_pub_key* pub)
{
	crypto_random(client->rsa.challenge, client->rsa.challenge_len);

	uint8_t buffer[1024];
	struct rfb_rsa_aes_challenge_msg *msg =
		(struct rfb_rsa_aes_challenge_msg*)buffer;

	ssize_t len = crypto_rsa_encrypt(pub, msg->challenge,
			crypto_rsa_pub_key_length(client->rsa.pub),
			client->rsa.challenge, client->rsa.challenge_len);
	msg->length = htons(len);

	stream_write(client->net_stream, buffer, sizeof(*msg) + len, NULL, NULL);
	return 0;
}

static int on_rsa_aes_public_key(struct nvnc_client* client)
{
	struct rfb_rsa_aes_pub_key_msg* msg =
	        (void*)(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint32_t bit_length = ntohl(msg->length);
	size_t byte_length = UDIV_UP(bit_length, 8);

	if (client->buffer_len - client->buffer_index <
			sizeof(*msg) + byte_length * 2)
		return 0;

	const uint8_t* modulus = msg->modulus_and_exponent;
	const uint8_t* exponent = msg->modulus_and_exponent + byte_length;

	client->rsa.pub =
		crypto_rsa_pub_key_import(modulus, exponent, byte_length);
	assert(client->rsa.pub);

	update_min_rtt(client);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CHALLENGE;
	rsa_aes_send_challenge(client, client->rsa.pub);

	return sizeof(*msg) + byte_length * 2;
}

static size_t client_rsa_aes_hash_len(const struct nvnc_client* client)
{
	switch (client->rsa.hash_type) {
	case CRYPTO_HASH_SHA1: return 20;
	case CRYPTO_HASH_SHA256: return 32;
	default:;
	}
	abort();
	return 0;
}

static int on_rsa_aes_challenge(struct nvnc_client* client)
{
	struct rfb_rsa_aes_challenge_msg* msg =
	        (void*)(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < sizeof(*msg))
		return 0;

	uint16_t length = ntohs(msg->length);
	if (client->buffer_len - client->buffer_index < sizeof(*msg) + length)
		return 0;

	struct nvnc* server = client->server;

	uint8_t client_random[32] = {};
	ssize_t len = crypto_rsa_decrypt(server->rsa_priv, client_random,
			client->rsa.challenge_len, msg->challenge, length);
	if (len < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to decrypt client's challenge");
		client->state = VNC_CLIENT_STATE_ERROR;
		nvnc_client_close(client);
		return -1;
	}

	// ClientSessionKey = the first 16 bytes of SHA1(ServerRandom || ClientRandom)
	uint8_t client_session_key[32];
	crypto_hash_many(client_session_key, client_rsa_aes_hash_len(client),
			client->rsa.hash_type, (const struct crypto_data_entry[]) {
		{ client->rsa.challenge, client->rsa.challenge_len },
		{ client_random, client->rsa.challenge_len },
		{}
	});

	// ServerSessionKey = the first 16 bytes of SHA1(ClientRandom || ServerRandom)
	uint8_t server_session_key[32];
	crypto_hash_many(server_session_key, client_rsa_aes_hash_len(client),
			client->rsa.hash_type, (const struct crypto_data_entry[]) {
		{ client_random, client->rsa.challenge_len },
		{ client->rsa.challenge, client->rsa.challenge_len },
		{}
	});

	stream_upgrade_to_rsa_eas(client->net_stream, client->rsa.cipher_type,
			server_session_key, client_session_key);

	size_t server_key_len = crypto_rsa_pub_key_length(server->rsa_pub);
	uint8_t* server_modulus = malloc(server_key_len * 2);
	uint8_t* server_exponent = server_modulus + server_key_len;

	crypto_rsa_pub_key_modulus(server->rsa_pub, server_modulus,
			server_key_len);
	crypto_rsa_pub_key_exponent(server->rsa_pub, server_exponent,
			server_key_len);

	size_t client_key_len = crypto_rsa_pub_key_length(client->rsa.pub);
	uint8_t* client_modulus = malloc(client_key_len * 2);
	uint8_t* client_exponent = client_modulus + client_key_len;

	crypto_rsa_pub_key_modulus(client->rsa.pub, client_modulus,
			client_key_len);
	crypto_rsa_pub_key_exponent(client->rsa.pub, client_exponent,
			client_key_len);

	uint32_t server_key_len_be = htonl(server_key_len * 8);
	uint32_t client_key_len_be = htonl(client_key_len * 8);

	uint8_t server_hash[32] = {};
	crypto_hash_many(server_hash, client_rsa_aes_hash_len(client),
			client->rsa.hash_type, (const struct crypto_data_entry[]) {
		{ (uint8_t*)&server_key_len_be, 4 },
		{ server_modulus, server_key_len },
		{ server_exponent, server_key_len },
		{ (uint8_t*)&client_key_len_be, 4 },
		{ client_modulus, client_key_len },
		{ client_exponent, client_key_len },
		{}
	});

	free(server_modulus);
	free(client_modulus);

	update_min_rtt(client);

	stream_write(client->net_stream, server_hash,
			client_rsa_aes_hash_len(client), NULL, NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CLIENT_HASH;

	return sizeof(*msg) + length;
}

static int on_rsa_aes_client_hash(struct nvnc_client* client)
{
	const char* msg = (void*)(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < client_rsa_aes_hash_len(client))
		return 0;

	struct nvnc* server = client->server;

	size_t server_key_len = crypto_rsa_pub_key_length(server->rsa_pub);
	uint8_t* server_modulus = malloc(server_key_len * 2);
	uint8_t* server_exponent = server_modulus + server_key_len;
	crypto_rsa_pub_key_modulus(server->rsa_pub, server_modulus,
			server_key_len);
	crypto_rsa_pub_key_exponent(server->rsa_pub, server_exponent,
			server_key_len);

	size_t client_key_len = crypto_rsa_pub_key_length(client->rsa.pub);
	uint8_t* client_modulus = malloc(client_key_len * 2);
	uint8_t* client_exponent = client_modulus + client_key_len;

	crypto_rsa_pub_key_modulus(client->rsa.pub, client_modulus,
			client_key_len);
	crypto_rsa_pub_key_exponent(client->rsa.pub, client_exponent,
			client_key_len);

	uint32_t server_key_len_be = htonl(server_key_len * 8);
	uint32_t client_key_len_be = htonl(client_key_len * 8);

	uint8_t client_hash[32] = {};
	crypto_hash_many(client_hash, client_rsa_aes_hash_len(client),
			client->rsa.hash_type, (const struct crypto_data_entry[]) {
		{ (uint8_t*)&client_key_len_be, 4 },
		{ client_modulus, client_key_len },
		{ client_exponent, client_key_len },
		{ (uint8_t*)&server_key_len_be, 4 },
		{ server_modulus, server_key_len },
		{ server_exponent, server_key_len },
		{}
	});

	free(client_modulus);
	free(server_modulus);

	if (memcmp(msg, client_hash, client_rsa_aes_hash_len(client)) != 0) {
		nvnc_log(NVNC_LOG_INFO, "Client hash mismatch");
		nvnc_client_close(client);
		return -1;
	}

	update_min_rtt(client);

	// TODO: Read this from config
	uint8_t subtype = RFB_RSA_AES_CRED_SUBTYPE_USER_AND_PASS;
	stream_write(client->net_stream, &subtype, 1, NULL, NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CREDENTIALS;
	return client_rsa_aes_hash_len(client);
}

static int on_rsa_aes_credentials(struct nvnc_client* client)
{
	const uint8_t* msg = (void*)(client->msg_buffer + client->buffer_index);

	if (client->buffer_len - client->buffer_index < 2)
		return 0;

	size_t username_len = msg[0];
	if (client->buffer_len - client->buffer_index < 2 + username_len)
		return 0;

	size_t password_len = msg[1 + username_len];
	if (client->buffer_len - client->buffer_index < 2 + username_len +
			password_len)
		return 0;

	struct nvnc* server = client->server;

	char username[256];
	char password[256];

	memcpy(username, (const char*)(msg + 1), username_len);
	username[username_len] = '\0';
	memcpy(password, (const char*)(msg + 2 + username_len), password_len);
	password[password_len] = '\0';

	update_min_rtt(client);

	if (server->auth_fn(username, password, server->auth_ud)) {
		security_handshake_ok(client, username);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		security_handshake_failed(client, username,
				"Invalid username or password");
	}

	return 2 + username_len + password_len;
}

int rsa_aes_handle_message(struct nvnc_client* client)
{
	switch (client->state) {
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_PUBLIC_KEY:
		return on_rsa_aes_public_key(client);
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CHALLENGE:
		return on_rsa_aes_challenge(client);
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CLIENT_HASH:
		return on_rsa_aes_client_hash(client);
	case VNC_CLIENT_STATE_WAITING_FOR_RSA_AES_CREDENTIALS:
		return on_rsa_aes_credentials(client);
	default:;
	}
	nvnc_log(NVNC_LOG_PANIC, "Unhandled client state: %d", client->state);
	return 0;
}
