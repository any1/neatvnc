#include <string.h>

#include "rfb-proto.h"

struct vnc_fb {
	uint8_t *data;
	uint32_t pixfmt;
	uint32_t width;
	uint32_t height;
};

struct rgb_pixfmt {
	int bits_per_pixel;
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
};

void convert_rgb_to_rgb(uint8_t * __restrict__ dst, const struct rgb_pixfmt *dstfmt,
			const uint8_t * __restrict__ src, const struct rgb_pixfmt *srcfmt,
			size_t len)
{
	int dst_bpp = dstfmt->bits_per_pixel / 8;
	int src_bpp = srcfmt->bits_per_pixel / 8;

	int sri = ffs(srcfmt->red_mask) - 1;
	int sgi = ffs(srcfmt->green_mask) - 1;
	int sbi = ffs(srcfmt->blue_mask) - 1;

	int dri = ffs(dstfmt->red_mask) - 1;
	int dgi = ffs(dstfmt->green_mask) - 1;
	int dbi = ffs(dstfmt->blue_mask) - 1;

	int srl = ffs(~(srcfmt->red_mask >> sri)) - 1;
	int sgl = ffs(~(srcfmt->green_mask >> sgi)) - 1;
	int sbl = ffs(~(srcfmt->blue_mask >> sbi)) - 1;

	int drl = ffs(~(dstfmt->red_mask >> dri)) - 1;
	int dgl = ffs(~(dstfmt->green_mask >> dgi)) - 1;
	int dbl = ffs(~(dstfmt->blue_mask >> dbi)) - 1;

	uint32_t dst_mask =
		dstfmt->red_mask | dstfmt->green_mask | dstfmt->blue_mask;

		const uint32_t * __restrict__ s = (const uint32_t*)(src);
		uint32_t * __restrict__ d = (uint32_t*)(dst);
	for (size_t i = 0; i < len; ++i) {
		uint32_t x = s[i];

		d[i] =
			((((x & dstfmt->red_mask) >> sri) << drl >> srl) << dri) |
			((((x & dstfmt->green_mask) >> sgi) << dgl >> sgl) << dgi) |
			((((x & dstfmt->blue_mask) >> sbi) << dbl >> sbl) << dbi);

	}

	/*
	for (size_t i = 0; i < len; ++i) {
//		uint32_t *s = (uint32_t*)(src + i * src_bpp);
//		uint32_t *d = (uint32_t*)(dst + i * dst_bpp);



//		*d &= ~dst_mask;
//		*d |= ((*s >> sri) * drm / srm) << dri;
//		*d |= ((*s >> sgi) * dgm / sgm) << dgi;
//		*d |= ((*s >> sbi) * dbm / sbm) << dbi;
	}
	*/
}

/*
#define X(sname, dname, r, g, b) \
void sname ## _to_ ## dname(uint8_t *dst, const uint8_t *src, size_t len) \
{ \
	for (int i = 0; i < len; ++i) { \
		dst[i * 3 + 0] = src[i * 4 + r]; \
		dst[i * 3 + 1] = src[i * 4 + g]; \
		dst[i * 3 + 2] = src[i * 4 + b]; \
	} \
}

X(RGBX8888, RGB888, 0, 1, 2)
X(BGRX8888, RGB888, 2, 1, 0)
X(XRGB8888, RGB888, 1, 2, 3)
X(XBGR8888, RGB888, 3, 2, 1)
#undef X

#define X(sname, dname, r, g, b) \
void sname ## _to_ ## dname(uint8_t *dst, const uint8_t *src, size_t len) \
{ \
	uint32_t *x = (uint32_t*)src; \
 \
	for (int i = 0; i < len; ++i) { \
		dst[i * 3 + 0] = (x[i] >> (r + 2)) & 0xff; \
		dst[i * 3 + 1] = (x[i] >> (g + 2)) & 0xff; \
		dst[i * 3 + 2] = (x[i] >> (b + 2)) & 0xff; \
	} \
}

X(RGBX1010102, RGB888, 22, 12, 2)
X(BGRX1010102, RGB888, 2, 12, 22)
X(XRGB1010102, RGB888, 20, 10, 0)
X(XBGR1010102, RGB888, 0, 10, 20)

#undef X
*/

void rfb_zrle_encode_tile(uint8_t *dst, uint32_t dst_pixfmt,
			  const struct vnc_fb *src)
{
	
}
