#pragma once

#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <gmp.h>

static inline void crypto_import(mpz_t n, const uint8_t* src, size_t len)
{
	int order = 1;
	int unit_size = 1;
	int endian = 1;
	int skip_bits = 0;

	mpz_import(n, len, order, unit_size, endian, skip_bits, src);
}

static inline size_t crypto_export(uint8_t* dst, size_t dst_size, const mpz_t n)
{
	int order = 1; // msb first
	int unit_size = 1; // byte
	int endian = 1; // msb first
	int skip_bits = 0;

	size_t bitsize = mpz_sizeinbase(n, 2);
	size_t bytesize = (bitsize + 7) / 8;

	assert(bytesize <= dst_size);

	memset(dst, 0, dst_size);
	mpz_export(dst + dst_size - bytesize, &bytesize, order, unit_size,
			endian, skip_bits, n);

	return bytesize;
}
