/*
 * Copyright (c) 2019 Andri Yngvason
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

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

struct vec {
	void* data;
	size_t len;
	size_t cap;
};

static inline void vec_clear(struct vec* vec)
{
	vec->len = 0;
}

int vec_init(struct vec* vec, size_t cap);
void vec_destroy(struct vec* vec);

int vec_reserve(struct vec* vec, size_t size);

void vec_bzero(struct vec* vec);

int vec_assign(struct vec* vec, const void* data, size_t size);
int vec_append(struct vec* vec, const void* data, size_t size);
void* vec_append_zero(struct vec* vec, size_t size);

static inline void vec_fast_append_8(struct vec* vec, uint8_t value)
{
        assert(vec->len < vec->cap);
        ((uint8_t*)vec->data)[vec->len++] = value;
}

static inline void vec_fast_append_32(struct vec* vec, uint32_t value)
{
        assert(vec->len + sizeof(value) <= vec->cap);
        assert(vec->len % sizeof(value) == 0);
        ((uint32_t*)vec->data)[vec->len] = value;
        vec->len += sizeof(value);
}

#define vec_for(elem, vec) \
	for (elem = (vec)->data; \
	     ((ptrdiff_t)elem - (ptrdiff_t)(vec)->data) < (ptrdiff_t)(vec)->len; \
	     ++elem)

#define vec_for_tail(elem, vec) \
	for (elem = (vec)->data, ++elem; \
	     ((ptrdiff_t)elem - (ptrdiff_t)(vec)->data) < (ptrdiff_t)(vec)->len; \
	     ++elem)

#define vec_for_ptr(elem, vec) \
	__typeof__(elem)* ptr_; \
	vec_for(ptr_, vec) \
		if ((elem = *ptr_))
