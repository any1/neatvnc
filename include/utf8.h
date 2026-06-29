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

#pragma once

#include <unistd.h>

#define LATIN1_TO_UTF8_MAX_SIZE(x) ((size_t)(x) * 2 + 1)
#define UTF8_TO_LATIN1_MAX_SIZE(x) ((size_t)(x) + 1)

/* NUL-terminates dst; returns the length excluding the terminator. */
size_t latin1_to_utf8(char* dst, const char* src, size_t src_len);

/* Code points above 0xff are replaced with '?'. NUL-terminates dst; returns
 * the length excluding the terminator.
 */
size_t utf8_to_latin1(char* dst, const char* src, size_t src_len);
