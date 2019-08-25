#include "vec.h"
#include "likely.h"

#include <stdlib.h>
#include <string.h>

int vec_init(struct vec* vec, size_t cap)
{
	memset(vec, 0, sizeof(*vec));
	return vec_reserve(vec, cap);
}

void vec_destroy(struct vec* vec)
{
	free(vec->data);
}

int vec_reserve(struct vec* vec, size_t size)
{
	if (likely(size <= vec->cap))
		return 0;

	void* data = realloc(vec->data, size);
	if (unlikely(!data))
		return -1;

	vec->cap = size;
	vec->data = data;

	return 0;
}

static int vec__grow(struct vec* vec, size_t size)
{
	if (likely(vec->len + size < vec->cap))
		return 0;
	
	return vec_reserve(vec, 2 * (vec->len + size));
}

int vec_assign(struct vec* vec, const void* data, size_t size)
{
	vec->len = 0;

	if (unlikely(vec_reserve(vec, size) < 0))
		return -1;

	vec->len = size;
	memcpy(vec->data, data, size);

	return 0;
}

int vec_append(struct vec* vec, const void* data, size_t size)
{
	if (unlikely(vec__grow(vec, size) < 0))
		return -1;

	char* p = vec->data;
	memcpy(&p[vec->len], data, size);
	vec->len += size;

	return 0;
}

void* vec_append_zero(struct vec* vec, size_t size)
{
	if (unlikely(vec__grow(vec, size) < 0))
		return NULL;

	char* p = vec->data;
	void* r = &p[vec->len];
	memset(r, 0, size);
	vec->len += size;

	return r;
}

void vec_bzero(struct vec* vec)
{
	memset(vec->data, 0, vec->cap);
}
