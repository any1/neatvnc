#include "pixels.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <libdrm/drm_fourcc.h>

#define swap32(x) (((x >> 24) & 0xff) | ((x << 8) & 0xff0000) | \
		((x >> 8) & 0xff00) | ((x << 24) & 0xff000000))

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define u32_le(x) x
#else
#define u32_le(x) swap32(x)
#endif

#define XSTR(s) STR(s)
#define STR(s) #s

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ARRAY_LEN(a) (sizeof(a) / (sizeof(a[0])))

static bool test_fourcc_to_pixman_fmt(void)
{
	pixman_format_code_t r;

#define check(a, b) \
	r = 0; \
	if (!fourcc_to_pixman_fmt(&r, b) || a != r) { \
		fprintf(stderr, "Failed check for %s\n", XSTR(a)); \
		return false; \
	} while(0)

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	check(PIXMAN_a2r10g10b10, DRM_FORMAT_ARGB2101010);
	check(PIXMAN_r8g8b8a8, DRM_FORMAT_RGBA8888);
	check(PIXMAN_b8g8r8a8, DRM_FORMAT_BGRA8888);
	check(PIXMAN_r5g6b5, DRM_FORMAT_RGB565);
#else
	check(PIXMAN_r8g8b8a8, DRM_FORMAT_ABGR8888);
	check(PIXMAN_b8g8r8a8, DRM_FORMAT_ARGB8888);
	check(PIXMAN_r5g6b5, DRM_FORMAT_BGR565);
#endif

#undef check
	return true;
}

static bool test_extract_alpha_mask_rgba8888(void)
{
	uint32_t test_data[] = {
		u32_le(0x00000000), // Black, transparent
		u32_le(0xff000000), // Red, transparent
		u32_le(0x00ff0000), // Red, transparent
		u32_le(0x0000ff00), // Green, transparent
		u32_le(0x000000ff), // Black, opaque
		u32_le(0xff0000ff), // Red, opaque
		u32_le(0x00ff00ff), // Red, opaque
		u32_le(0x0000ffff), // Green, opaque
	};

	uint8_t mask[UDIV_UP(ARRAY_LEN(test_data), 8)] = { 0 };

	bool ok = extract_alpha_mask(mask, test_data, DRM_FORMAT_RGBA8888,
			ARRAY_LEN(test_data));
	if (!ok) {
		fprintf(stderr, "Failed to extract alpha mask");
		return false;
	}

	if (mask[0] != 0x0f) {
		fprintf(stderr, "Expected alpha mask to be 0b00001111 (0x0f), but it was %x",
				mask[0]);
		return false;
	}

	memset(mask, 0, sizeof(mask));

	ok = extract_alpha_mask(mask, test_data, DRM_FORMAT_BGRA8888,
			ARRAY_LEN(test_data));
	if (!ok) {
		fprintf(stderr, "Failed to extract alpha mask");
		return false;
	}

	if (mask[0] != 0x0f) {
		fprintf(stderr, "Expected alpha mask to be 0b00001111 (0x0f), but it was %x",
				mask[0]);
		return false;
	}

	return true;
}

int main()
{
	bool ok = test_fourcc_to_pixman_fmt() &&
		test_extract_alpha_mask_rgba8888();
	return ok ? 0 : 1;
}
