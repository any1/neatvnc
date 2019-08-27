#pragma once

#include <stddef.h>

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
