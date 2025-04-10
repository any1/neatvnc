#include "common.h"
#include "stream/stream.h"
#include "auth/auth.h"
#include "auth/vnc_auth.h"

void vnc_auth_reverse_bits(uint8_t *dst, uint8_t *src)
{
	for (int i = 0; i < VNC_AUTH_PASSWORD_LEN; ++i) {
		uint8_t b = src[i];
		b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
		b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
		b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
		dst[i] = b;
	}
}

int vnc_auth_send_challenge(struct nvnc_client* client)
{
	crypto_random(client->vnc_auth_challenge, VNC_AUTH_CHALLENGE_LEN);

	stream_write(client->net_stream, client->vnc_auth_challenge,
			VNC_AUTH_CHALLENGE_LEN, NULL, NULL);

	return 0;
}

int vnc_auth_handle_response(struct nvnc_client* client)
{
	uint8_t* msg = client->msg_buffer + client->buffer_index;

	if (client->buffer_len - client->buffer_index < VNC_AUTH_RESPONSE_LEN)
		return 0;

	struct nvnc* server = client->server;

	uint8_t hash[VNC_AUTH_RESPONSE_LEN] = {0};
	crypto_des_encrypt(server->vnc_auth_password, hash, client->vnc_auth_challenge,
			VNC_AUTH_CHALLENGE_LEN);

	if (strncmp((char*)hash, (char*)msg, VNC_AUTH_RESPONSE_LEN) == 0) {
		security_handshake_ok(client, NULL);
		client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	} else {
		security_handshake_failed(client, NULL, "Invalid password");
	}

	return VNC_AUTH_RESPONSE_LEN;
}
