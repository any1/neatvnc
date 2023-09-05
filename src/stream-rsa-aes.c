/*
 * Copyright (c) 2023 Andri Yngvason
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

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/param.h>
#include <arpa/inet.h>

#include "rcbuf.h"
#include "stream.h"
#include "stream-tcp.h"
#include "stream-common.h"
#include "crypto.h"
#include "neatvnc.h"

#define RSA_AES_BUFFER_SIZE 8192
#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

struct stream_rsa_aes {
	struct stream base;

	size_t read_index;
	uint8_t* read_buffer;

	struct crypto_cipher* cipher;
};

static_assert(sizeof(struct stream_rsa_aes) <= STREAM_ALLOC_SIZE,
		"struct stream_rsa_aes has grown too large, increase STREAM_ALLOC_SIZE");

static void stream_rsa_aes_destroy(struct stream* base)
{
	struct stream_rsa_aes* self = (struct stream_rsa_aes*)base;
	crypto_cipher_del(self->cipher);
	free(self->read_buffer);
	stream_tcp_destroy(base);
}

static void stream_rsa_aes_read_into_buffer(struct stream_rsa_aes* self)
{
	ssize_t n_read = stream_tcp_read(&self->base,
			self->read_buffer + self->read_index,
			RSA_AES_BUFFER_SIZE - self->read_index);
	if (n_read > 0)
		self->read_index += n_read;
}

static ssize_t stream_rsa_aes_parse_header(struct stream_rsa_aes* self)
{
	if (self->read_index <= 2) {
		return -1;
	}

	uint16_t len_be;
	memcpy(&len_be, self->read_buffer, sizeof(len_be));
	size_t len = ntohs(len_be);

	if (self->read_index < 2 + 16 + len) {
		return -1;
	}

	return len;
}

static ssize_t stream_rsa_aes_read_message(struct stream_rsa_aes* self,
		uint8_t* dst, size_t size)
{
	ssize_t msg_len = stream_rsa_aes_parse_header(self);
	if (msg_len < 0) {
		return 0;
	}

	// The entire message must fit in dst
	/* TODO: With this, stream_tcp__on_event won't run until network input
	 * is received. We need to somehow schedule on_event or also buffer the
	 * decrypted data here.
	 * Another option would be to keep back the message counter in the
	 * cipher until the message has been fully read.
	 */
	if ((size_t)msg_len > size)
		return 0;

	uint16_t msg_len_be = htons(msg_len);

	uint8_t expected_mac[16];
	ssize_t n = crypto_cipher_decrypt(self->cipher, dst, expected_mac,
			self->read_buffer + 2, msg_len,
			(uint8_t*)&msg_len_be, sizeof(msg_len_be));

	uint8_t* actual_mac = self->read_buffer + 2 + msg_len;
	if (memcmp(expected_mac, actual_mac, 16) != 0) {
		nvnc_log(NVNC_LOG_DEBUG, "Message authentication failed");
		errno = EBADMSG;
		return -1;
	}

	self->read_index -= 2 + 16 + msg_len;
	memmove(self->read_buffer, self->read_buffer + 2 + 16 + msg_len,
			self->read_index);

	return n;
}

static ssize_t stream_rsa_aes_read(struct stream* base, void* dst, size_t size)
{
	struct stream_rsa_aes* self = (struct stream_rsa_aes*)base;

	stream_rsa_aes_read_into_buffer(self);
	if (self->base.state == STREAM_STATE_CLOSED)
		return 0;

	size_t total_read = 0;

	while (true) {
		ssize_t n_read = stream_rsa_aes_read_message(self, dst, size);
		if (n_read == 0)
			break;

		if (n_read < 0) {
			if (errno == EAGAIN) {
				break;
			}
			return -1;
		}

		total_read += n_read;
		dst += n_read;
		size -= n_read;
	}

	return total_read;
}

static int stream_rsa_aes_send(struct stream* base, struct rcbuf* payload,
                stream_req_fn on_done, void* userdata)
{
	struct stream_rsa_aes* self = (struct stream_rsa_aes*)base;
	size_t n_msg = UDIV_UP(payload->size, RSA_AES_BUFFER_SIZE);

	struct vec buf;
	vec_init(&buf, payload->size + n_msg * (2 + 16));

	for (size_t i = 0; i < n_msg; ++i) {
		size_t msglen = MIN(payload->size - i * RSA_AES_BUFFER_SIZE,
				RSA_AES_BUFFER_SIZE);
		uint16_t msglen_be = htons(msglen);

		vec_append(&buf, &msglen_be, sizeof(msglen_be));

		uint8_t mac[16];
		crypto_cipher_encrypt(self->cipher, &buf, mac,
				payload->payload + i * RSA_AES_BUFFER_SIZE,
				msglen, (uint8_t*)&msglen_be, sizeof(msglen_be));
		vec_append(&buf, mac, sizeof(mac));
	}

	int r = stream_tcp_send(base, rcbuf_new(buf.data, buf.len), on_done,
			userdata);
	if (r < 0) {
		return r;
	}

	return payload->size;
}

static struct stream_impl impl = {
	.close = stream_tcp_close,
	.destroy = stream_rsa_aes_destroy,
	.read = stream_rsa_aes_read,
	.send = stream_rsa_aes_send,
};

int stream_upgrade_to_rsa_eas(struct stream* base,
		enum crypto_cipher_type cipher_type,
		const uint8_t* enc_key, const uint8_t* dec_key)
{
	struct stream_rsa_aes* self = (struct stream_rsa_aes*)base;

	self->read_index = 0;
	self->read_buffer = malloc(RSA_AES_BUFFER_SIZE);
	if (!self->read_buffer)
		return -1;

	self->cipher = crypto_cipher_new(enc_key, dec_key, cipher_type);
	if (!self->cipher) {
		free(self->read_buffer);
		return -1;
	}

	self->base.impl = &impl;
	return 0;
}
