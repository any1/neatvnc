#include "pixels.h"
#include "rfb-proto.h"

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

static bool test_pixel_to_cpixel_4bpp(void)
{
	uint32_t src = u32_le(0x11223344u);
	uint32_t dst;
	uint8_t* src_addr = (uint8_t*)&src;

	struct rfb_pixel_format dstfmt = { 0 }, srcfmt = { 0 };

	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_RGBA8888);

	dst = 0;
	rfb_pixfmt_from_fourcc(&srcfmt, DRM_FORMAT_RGBA8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if ((src & 0xffffff00u) != (dst & 0xffffff00u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_ABGR8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x00332211u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_ARGB8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x00112233u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_BGRA8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x33221100u))
		return false;

	return true;
}

static bool test_pixel_to_cpixel_3bpp(void)
{
	//44 is extra data that should not be copied anywhere below.
	uint32_t src = u32_le(0x44112233u);
	uint32_t dst;
	uint8_t* src_addr = (uint8_t*)&src;

	struct rfb_pixel_format dstfmt = { 0 }, srcfmt = { 0 };

	rfb_pixfmt_from_fourcc(&srcfmt, DRM_FORMAT_RGB888);

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_RGBA8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x11223300u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_ABGR8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x00332211u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_ARGB8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x00112233u))
		return false;

	dst = 0;
	rfb_pixfmt_from_fourcc(&dstfmt, DRM_FORMAT_BGRA8888);
	pixel_to_cpixel((uint8_t*)&dst, &dstfmt, src_addr, &srcfmt, 4, 1);
	if (dst != u32_le(0x33221100u))
		return false;

	return true;
}

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
		u32_le(0x00000000u), // Black, transparent
		u32_le(0xff000000u), // Red, transparent
		u32_le(0x00ff0000u), // Red, transparent
		u32_le(0x0000ff00u), // Green, transparent
		u32_le(0x000000ffu), // Black, opaque
		u32_le(0xff0000ffu), // Red, opaque
		u32_le(0x00ff00ffu), // Red, opaque
		u32_le(0x0000ffffu), // Green, opaque
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

static bool test_drm_format_to_string(void)
{
	if (strcmp(drm_format_to_string(DRM_FORMAT_RGBA8888), "RGBA8888") != 0)
		return false;

	if (strcmp(drm_format_to_string(DRM_FORMAT_RGBX8888), "RGBX8888") != 0)
		return false;

	if (strcmp(drm_format_to_string(DRM_FORMAT_RGB565), "RGB565") != 0)
		return false;

	return true;
}

static bool test_rfb_pixfmt_to_string(void)
{
	struct rfb_pixel_format rgbx8888;
	struct rfb_pixel_format bgrx8888;
	struct rfb_pixel_format xrgb8888;
	struct rfb_pixel_format xbgr8888;

	rfb_pixfmt_from_fourcc(&rgbx8888, DRM_FORMAT_RGBX8888);
	rfb_pixfmt_from_fourcc(&bgrx8888, DRM_FORMAT_BGRX8888);
	rfb_pixfmt_from_fourcc(&xrgb8888, DRM_FORMAT_XRGB8888);
	rfb_pixfmt_from_fourcc(&xbgr8888, DRM_FORMAT_XBGR8888);

	if (strcmp(rfb_pixfmt_to_string(&rgbx8888), "RGBX8888") != 0)
		return false;

	if (strcmp(rfb_pixfmt_to_string(&bgrx8888), "BGRX8888") != 0)
		return false;

	if (strcmp(rfb_pixfmt_to_string(&xrgb8888), "XRGB8888") != 0)
		return false;

	if (strcmp(rfb_pixfmt_to_string(&xbgr8888), "XBGR8888") != 0)
		return false;

	return true;
}

int main()
{
	bool ok = test_pixel_to_cpixel_4bpp() &&
		test_pixel_to_cpixel_3bpp() &&
		test_fourcc_to_pixman_fmt() &&
		test_extract_alpha_mask_rgba8888() &&
		test_drm_format_to_string() &&
		test_rfb_pixfmt_to_string();
	return ok ? 0 : 1;
}
