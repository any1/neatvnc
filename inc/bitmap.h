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

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

struct bitmap {
	size_t n_elem;
	uint64_t data[0];
};

static inline struct bitmap *bitmap_alloc(size_t bitlen)
{
	size_t n_elem = UDIV_UP(bitlen, 64);

	struct bitmap *self = calloc(1, sizeof(*self) + n_elem * sizeof(*self->data));
	if (!self)
		return NULL;

	self->n_elem = n_elem;

	return self;
}

static inline void bitmap_clear_all(struct bitmap *self)
{
	for (size_t i = 0; i < self->n_elem; ++i)
		self->data[i] = 0;
}

static inline int bitmap_is_empty(const struct bitmap *self)
{
	for (size_t i = 0; i < self->n_elem; ++i)
		if (self->data[i])
			return 0;

	return 1;
}

static inline int bitmap_is_set(const struct bitmap *self, int index)
{
	return !!(self->data[index / 64] & (1ULL << (index % 64)));
}

static inline void bitmap_clear(struct bitmap* self, int index)
{
	self->data[index / 64] &= ~(1ULL << (index % 64));
}

static inline void bitmap_set_cond(struct bitmap* self, int index, bool cond)
{
	self->data[index / 64] |= ((uint64_t)cond) << (index % 64);
}

static inline void bitmap_set(struct bitmap* self, int index)
{
	bitmap_set_cond(self, index, true);
}

static inline int bitmap_runlength(const struct bitmap *self, int start)
{
        int r = 0;
        while (bitmap_is_set(self, start + r)) ++r;
        return r;
}
