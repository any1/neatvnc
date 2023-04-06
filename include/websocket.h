#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#define WS_HEADER_MIN_SIZE 14

enum ws_opcode {
	WS_OPCODE_CONT = 0,
	WS_OPCODE_TEXT,
	WS_OPCODE_BIN,
	WS_OPCODE_CLOSE = 8,
	WS_OPCODE_PING,
	WS_OPCODE_PONG,
};

struct ws_frame_header {
	bool fin;
	enum ws_opcode opcode;
	bool mask;
	uint64_t payload_length;
	uint8_t masking_key[4];
	size_t header_length;
};

ssize_t ws_handshake(char* output, size_t output_maxlen, const char* input);

const char *ws_opcode_name(enum ws_opcode op);

bool ws_parse_frame_header(struct ws_frame_header* header,
		const uint8_t* payload, size_t length);
void ws_apply_mask(const struct ws_frame_header* header,
		uint8_t* restrict payload);
void ws_copy_payload(const struct ws_frame_header* header,
		uint8_t* restrict dst, const uint8_t* restrict src, size_t len);
int ws_write_frame_header(uint8_t* dst, const struct ws_frame_header* header);
