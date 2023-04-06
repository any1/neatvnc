#include "websocket.h"

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

static inline uint64_t u64_from_network_order(uint64_t x)
{
#if __BYTE_ORDER__ == __BIG_ENDIAN__
	return x;
#else
	return __builtin_bswap64(x);
#endif
}

static inline uint64_t u64_to_network_order(uint64_t x)
{
#if __BYTE_ORDER__ == __BIG_ENDIAN__
	return x;
#else
	return __builtin_bswap64(x);
#endif
}

const char *ws_opcode_name(enum ws_opcode op)
{
	switch (op) {
	case WS_OPCODE_CONT: return "cont";
	case WS_OPCODE_TEXT: return "text";
	case WS_OPCODE_BIN: return "bin";
	case WS_OPCODE_CLOSE: return "close";
	case WS_OPCODE_PING: return "ping";
	case WS_OPCODE_PONG: return "pong";
	}
	return "INVALID";
}

bool ws_parse_frame_header(struct ws_frame_header* header,
		const uint8_t* payload, size_t length)
{
	if (length < 2)
		return false;

	int i = 0;

	header->fin = !!(payload[i] & 0x80);
	header->opcode = (payload[i++] & 0x0f);
	header->mask = !!(payload[i] & 0x80);
	header->payload_length = payload[i++] & 0x7f;

	if (header->payload_length == 126) {
		if (length - i < 2)
			return false;

		uint16_t value = 0;
		memcpy(&value, &payload[i], 2);
		header->payload_length = ntohs(value);
		i += 2;
	} else if (header->payload_length == 127) {
		if (length - i < 8)
			return false;

		uint64_t value = 0;
		memcpy(&value, &payload[i], 8);
		header->payload_length = u64_from_network_order(value);
		i += 8;
	}

	if (header->mask) {
		if (length - i < 4)
			return false;

		memcpy(header->masking_key, &payload[i], 4);
		i += 4;
	}

	header->header_length = i;

	return true;
}

void ws_apply_mask(const struct ws_frame_header* header,
		uint8_t* restrict payload)
{
	assert(header->mask);

	uint64_t len = header->payload_length;
	const uint8_t* restrict key = header->masking_key;

	for (uint64_t i = 0; i < len; ++i) {
		payload[i] ^= key[i % 4];
	}
}

void ws_copy_payload(const struct ws_frame_header* header,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len)
{
	if (!header->mask) {
		memcpy(dst, src, len);
		return;
	}

	const uint8_t* restrict key = header->masking_key;
	for (uint64_t i = 0; i < len; ++i) {
		dst[i] = src[i] ^ key[i % 4];
	}
}

int ws_write_frame_header(uint8_t* dst, const struct ws_frame_header* header)
{
	int i = 0;
	dst[i++] = ((uint8_t)header->fin << 7) | (header->opcode);

	if (header->payload_length <= 125) {
		dst[i++] = ((uint8_t)header->mask << 7) | header->payload_length;
	} else if (header->payload_length <= UINT16_MAX) {
		dst[i++] = ((uint8_t)header->mask << 7) | 126;
		uint16_t be = htons(header->payload_length);
		memcpy(&dst[i], &be, 2);
		i += 2;
	} else {
		dst[i++] = ((uint8_t)header->mask << 7) | 127;
		uint64_t be = u64_to_network_order(header->payload_length);
		memcpy(&dst[i], &be, 8);
		i += 8;
	}

	if (header->mask) {
		memcpy(dst, header->masking_key, 4);
		i += 4;
	}

	return i;
}
