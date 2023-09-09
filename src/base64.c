/* Copyright (c) 2023 Andri Yngvason
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

#include "base64.h"

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char base64_enc_lut[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t base64_validation_lut[256] = {
	['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1,
	['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1,
	['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1,
	['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1,
	['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1,
	['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
	['Y'] = 1, ['Z'] = 1, ['a'] = 1, ['b'] = 1,
	['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
	['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1,
	['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1,
	['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1,
	['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1,
	['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1,
	['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1,
	['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1,
	['8'] = 1, ['9'] = 1, ['+'] = 1, ['/'] = 1,
	['-'] = 1, ['_'] = 1, ['='] = 1,
};

static const uint8_t base64_dec_lut[256] = {
	['A'] = 0x00, ['B'] = 0x01, ['C'] = 0x02, ['D'] = 0x03,
	['E'] = 0x04, ['F'] = 0x05, ['G'] = 0x06, ['H'] = 0x07,
	['I'] = 0x08, ['J'] = 0x09, ['K'] = 0x0a, ['L'] = 0x0b,
	['M'] = 0x0c, ['N'] = 0x0d, ['O'] = 0x0e, ['P'] = 0x0f,
	['Q'] = 0x10, ['R'] = 0x11, ['S'] = 0x12, ['T'] = 0x13,
	['U'] = 0x14, ['V'] = 0x15, ['W'] = 0x16, ['X'] = 0x17,
	['Y'] = 0x18, ['Z'] = 0x19, ['a'] = 0x1a, ['b'] = 0x1b,
	['c'] = 0x1c, ['d'] = 0x1d, ['e'] = 0x1e, ['f'] = 0x1f,
	['g'] = 0x20, ['h'] = 0x21, ['i'] = 0x22, ['j'] = 0x23,
	['k'] = 0x24, ['l'] = 0x25, ['m'] = 0x26, ['n'] = 0x27,
	['o'] = 0x28, ['p'] = 0x29, ['q'] = 0x2a, ['r'] = 0x2b,
	['s'] = 0x2c, ['t'] = 0x2d, ['u'] = 0x2e, ['v'] = 0x2f,
	['w'] = 0x30, ['x'] = 0x31, ['y'] = 0x32, ['z'] = 0x33,
	['0'] = 0x34, ['1'] = 0x35, ['2'] = 0x36, ['3'] = 0x37,
	['4'] = 0x38, ['5'] = 0x39, ['6'] = 0x3a, ['7'] = 0x3b,
	['8'] = 0x3c, ['9'] = 0x3d, ['+'] = 0x3e, ['/'] = 0x3f,
	['-'] = 0x3e, ['_'] = 0x3f,
};

void base64_encode(char* dst, const uint8_t* src, size_t src_len)
{
	size_t i = 0;

	for (; i < src_len / 3; ++i) {
		uint32_t tmp = 0;
		tmp |= (uint32_t)src[i * 3 + 0] << 16;
		tmp |= (uint32_t)src[i * 3 + 1] << 8;
		tmp |= (uint32_t)src[i * 3 + 2];

		dst[i * 4 + 0] = base64_enc_lut[tmp >> 18];
		dst[i * 4 + 1] = base64_enc_lut[(tmp >> 12) & 0x3f];
		dst[i * 4 + 2] = base64_enc_lut[(tmp >> 6) & 0x3f];
		dst[i * 4 + 3] = base64_enc_lut[tmp & 0x3f];
	}

	size_t rem = src_len % 3;
	if (rem == 0) {
		dst[i * 4] = '\0';
		return;
	}

	uint32_t tmp = 0;
	for (size_t r = 0; r < rem; ++r) {
		size_t s = (2 - r) * 8;
		tmp |= (uint32_t)src[i * 3 + r] << s;
	}

	size_t di = 0;
	for (; di < rem + 1; ++di) {
		size_t s = (3 - di) * 6;
		dst[i * 4 + di] = base64_enc_lut[(tmp >> s) & 0x3f];
	}

	for (; di < 4; ++di) {
		dst[i * 4 + di] = '=';
	}

	dst[i * 4 + di] = '\0';

}

static bool base64_is_valid(const char* src)
{
	for (int i = 0; src[i]; i++)
		if (!base64_validation_lut[(uint8_t)src[i]])
			return false;
	return true;
}

ssize_t base64_decode(uint8_t* dst, const char* src)
{
	if (!base64_is_valid(src))
		return -1;

	size_t src_len = strcspn(src, "=");
	size_t i = 0;

	for (; i < src_len / 4; ++i) {
		uint32_t tmp = 0;
		tmp |= (uint32_t)base64_dec_lut[(uint8_t)src[i * 4 + 0]] << 18;
		tmp |= (uint32_t)base64_dec_lut[(uint8_t)src[i * 4 + 1]] << 12;
		tmp |= (uint32_t)base64_dec_lut[(uint8_t)src[i * 4 + 2]] << 6;
		tmp |= (uint32_t)base64_dec_lut[(uint8_t)src[i * 4 + 3]];

		dst[i * 3 + 0] = tmp >> 16;
		dst[i * 3 + 1] = (tmp >> 8) & 0xff;
		dst[i * 3 + 2] = tmp & 0xff;
	}

	size_t rem = src_len % 4;
	if (rem == 0)
		return i * 3;

	size_t di = 0;

	uint32_t tmp = 0;
	for (size_t r = 0; r < rem; ++r) {
		size_t s = (3 - r) * 6;
		tmp |= (uint32_t)base64_dec_lut[(uint8_t)src[i * 4 + r]] << s;
	}

	for (; di < (rem * 3) / 4; ++di) {
		size_t s = (2 - di) * 8;
		dst[i * 3 + di] = (tmp >> s) & 0xff;
	}

	return i * 3 + di;
}
