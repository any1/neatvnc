/*
 * Copyright (c) 2026 Norbert Schultz
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

#include "utf8.h"
#include <stdbool.h>
#include <stdint.h>

size_t latin1_to_utf8(char* dst, const char* src, size_t src_len)
{
	size_t o = 0;
	for (size_t i = 0; i < src_len; ++i) {
		uint8_t c = src[i];
		if (c < 0x80) {
			dst[o++] = c;
		} else {
			dst[o++] = 0xc0 | (c >> 6);
			dst[o++] = 0x80 | (c & 0x3f);
		}
	}

	dst[o] = 0;
	return o;
}

size_t utf8_to_latin1(char* dst, const char* src, size_t src_len)
{
	size_t o = 0;
	for (size_t i = 0; i < src_len;) {
		uint8_t c = src[i];
		uint32_t cp;
		uint32_t min_cp;
		size_t n;

		if (c < 0x80) {
			dst[o++] = c;
			i++;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			cp = c & 0x1f;
			n = 2;
			min_cp = 0x80;
		} else if ((c & 0xf0) == 0xe0) {
			cp = c & 0x0f;
			n = 3;
			min_cp = 0x800;
		} else if ((c & 0xf8) == 0xf0) {
			cp = c & 0x07;
			n = 4;
			min_cp = 0x10000;
		} else {
			/* Stray continuation byte or invalid lead byte. */
			dst[o++] = '?';
			i++;
			continue;
		}

		if (i + n > src_len) {
			/* Truncated sequence at end of input. */
			dst[o++] = '?';
			break;
		}

		bool valid = true;
		for (size_t k = 1; k < n; ++k) {
			uint8_t cc = src[i + k];
			if ((cc & 0xc0) != 0x80) {
				valid = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3f);
		}

		if (!valid) {
			/* Missing continuation byte; resync on the bad byte. */
			dst[o++] = '?';
			i++;
			continue;
		}

		/* Reject overlong encodings, UTF-16 surrogates and code points
		 * beyond the Unicode range.
		 */
		if (cp < min_cp || (cp >= 0xd800 && cp <= 0xdfff) ||
				cp > 0x10ffff)
			dst[o++] = '?';
		else
			dst[o++] = cp <= 0xff ? (char)cp : '?';

		i += n;
	}

	dst[o] = 0;
	return o;
}
