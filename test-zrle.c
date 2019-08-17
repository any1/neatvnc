#include "zrle.h"
#include "rfb-proto.h"

#include <endian.h>
#include <stdio.h>

int test_pixel32_to_cpixel_3(void)
{
	static uint32_t src[64] = { 0 };
	static uint8_t dst[64 * 3] = { 0 };

	struct rfb_pixel_format srcfmt = { 0 }, dstfmt = { 0 };

	srcfmt.depth = 24;
	srcfmt.bits_per_pixel = 32;
	srcfmt.true_colour_flag = 1;
	srcfmt.red_max = 255;
	srcfmt.green_max = 255;
	srcfmt.blue_max = 255;
	srcfmt.red_shift = 24;
	srcfmt.green_shift = 16;
	srcfmt.blue_shift = 8;

	dstfmt.depth = 24;
	dstfmt.bits_per_pixel = 32;
	dstfmt.true_colour_flag = 1;
	dstfmt.red_max = 255;
	dstfmt.green_max = 255;
	dstfmt.blue_max = 255;
	dstfmt.red_shift = 8;
	dstfmt.green_shift = 16;
	dstfmt.blue_shift = 24;

	for (int i = 0; i < 64; ++i)
		src[i] = 0x12345678;

	pixel32_to_cpixel(dst, &dstfmt, src, &srcfmt, 3, 64);

	for (int i = 0; i < 64; ++i) {
		uint8_t r, g, b;

		r = dst[i * 3 + 0];
		g = dst[i * 3 + 1];
		b = dst[i * 3 + 2];

		if (r != 0x12 || g != 0x34 || b != 0x56) {
			printf("pixel32_to_cpixel_3 failed\n");
			return 1;
		}
	}

	printf("pixel32_to_cpixel_3 ran OK\n");
	return 0;
}

int test_pixel32_to_cpixel_2(void)
{
	static uint32_t src[64] = { 0 };
	static uint16_t dst[64] = { 0 };

	struct rfb_pixel_format srcfmt = { 0 }, dstfmt = { 0 };

	srcfmt.depth = 24;
	srcfmt.bits_per_pixel = 32;
	srcfmt.true_colour_flag = 1;
	srcfmt.red_max = 255;
	srcfmt.green_max = 255;
	srcfmt.blue_max = 255;
	srcfmt.red_shift = 24;
	srcfmt.green_shift = 16;
	srcfmt.blue_shift = 8;

	dstfmt.depth = 16;
	dstfmt.bits_per_pixel = 16;
	dstfmt.true_colour_flag = 1;
	dstfmt.red_max = 31;
	dstfmt.green_max = 63;
	dstfmt.blue_max = 31;
	dstfmt.red_shift = 11;
	dstfmt.green_shift = 5;
	dstfmt.blue_shift = 0;

#if __BYTE_ORDER__ == __BIG_ENDIAN__
	dstfmt.big_endian_flag = 1;
#endif

	for (int i = 0; i < 64; ++i)
		src[i] = 0x12345678;

	pixel32_to_cpixel((uint8_t*)dst, &dstfmt, src, &srcfmt, 2, 64);

	for (int i = 0; i < 64; ++i) {
		uint8_t r, g, b;

		r = (dst[i] >> 11) & 31;
		g = (dst[i] >> 5) & 63;
		b = (dst[i] >> 0) & 31;

		if (r != (0x12 >> 3) || g != (0x34 >> 2) || b != (0x56 >> 3)) {
			printf("pixel32_to_cpixel_2 failed: %x %x %x\n", r, g, b);
			return 1;
		}
	}

	printf("pixel32_to_cpixel_2 ran OK\n");
	return 0;
}

int test_pixel32_to_cpixel_1(void)
{
	static uint32_t src[64] = { 0 };
	static uint8_t dst[64] = { 0 };

	struct rfb_pixel_format srcfmt = { 0 }, dstfmt = { 0 };

	srcfmt.depth = 24;
	srcfmt.bits_per_pixel = 32;
	srcfmt.true_colour_flag = 1;
	srcfmt.red_max = 255;
	srcfmt.green_max = 255;
	srcfmt.blue_max = 255;
	srcfmt.red_shift = 24;
	srcfmt.green_shift = 16;
	srcfmt.blue_shift = 8;

	dstfmt.depth = 8;
	dstfmt.bits_per_pixel = 8;
	dstfmt.true_colour_flag = 1;
	dstfmt.red_max = 7;
	dstfmt.green_max = 7;
	dstfmt.blue_max = 3;
	dstfmt.red_shift = 5;
	dstfmt.green_shift = 2;
	dstfmt.blue_shift = 0;

#if __BYTE_ORDER__ == __BIG_ENDIAN__
	dstfmt.big_endian_flag = 1;
#endif

	for (int i = 0; i < 64; ++i)
		src[i] = 0x98765432;

	pixel32_to_cpixel((uint8_t*)dst, &dstfmt, src, &srcfmt, 1, 64);

	for (int i = 0; i < 64; ++i) {
		uint8_t r, g, b;

		r = (dst[i] >> 5) & 7;
		g = (dst[i] >> 2) & 7;
		b = (dst[i] >> 0) & 3;

		if (r != (0x98 >> 5) || g != (0x76 >> 5) || b != (0x54 >> 6)) {
			printf("pixel32_to_cpixel_1 failed: %x %x %x\n", r, g, b);
			return 1;
		}
	}

	printf("pixel32_to_cpixel_1 ran OK\n");
	return 0;
}
int main()
{
	if (test_pixel32_to_cpixel_3()) return 1;
	if (test_pixel32_to_cpixel_2()) return 1;
	if (test_pixel32_to_cpixel_1()) return 1;

	return 0;
}
