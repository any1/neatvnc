/*
 * Copyright (c) 2019 Andri Yngvason
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
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
