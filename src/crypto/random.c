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

#include "crypto.h"
#include "config.h"

#include <stdint.h>

#if defined(HAVE_GETRANDOM)
#include <sys/random.h>
#elif defined(HAVE_ARC4RANDOM)
#include <stdlib.h>
#endif


void crypto_random(uint8_t* dst, size_t len)
{
#if defined(HAVE_GETRANDOM)
	getrandom(dst, len, 0);
#elif defined(HAVE_ARC4RANDOM)
	arc4random_buf(dst, len);
#else
#error No random generator available
#endif
}
