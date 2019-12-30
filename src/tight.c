#include "neatvnc.h"
#include "rfb-proto.h"
#include "vec.h"
#include "fb.h"
#include "tight.h"
#include "common.h"
#include "pixels.h"
#include "rfb-proto.h"

#include <pixman.h>
#include <turbojpeg.h>
#include <png.h>
#include <stdlib.h>
#include <libdrm/drm_fourcc.h>

#define TIGHT_FILL 0x80
#define TIGHT_JPEG 0x90
#define TIGHT_PNG 0xA0
#define TIGHT_BASIC 0x00

enum TJPF get_jpeg_pixfmt(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		return TJPF_XRGB;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		return TJPF_XBGR;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return TJPF_RGBX;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return TJPF_BGRX;
	}

	return TJPF_UNKNOWN;
}

static void tight_encode_size(struct vec* dst, size_t size)
{
	vec_fast_append_8(dst, (size & 0x7f) | ((size >= 128) << 7));
	if (size >= 128)
		vec_fast_append_8(dst, ((size >> 7) & 0x7f) | ((size >= 16384) << 7));
	if (size >= 16384)
		vec_fast_append_8(dst, (size >> 14) & 0x7f);
}

int tight_encode_box_jpeg(struct vec* dst, struct nvnc_client* client,
                          const struct nvnc_fb* fb, uint32_t x, uint32_t y,
                          uint32_t stride, uint32_t width, uint32_t height)
{

	unsigned char* buffer = NULL;
	size_t size = 0;

	int quality = 50; /* 1 - 100 */
	enum TJPF tjfmt = get_jpeg_pixfmt(fb->fourcc_format);
	if (tjfmt == TJPF_UNKNOWN)
		return -1;

	vec_reserve(dst, 4096);

	tjhandle handle = tjInitCompress();

	void* img = (uint32_t*)fb->addr + x + y * stride;

	tjCompress2(handle, img, width, stride * 4, height, tjfmt, &buffer,
	            &size, TJSAMP_422, quality, TJFLAG_FASTDCT);

	vec_fast_append_8(dst, TIGHT_JPEG);

	tight_encode_size(dst, size);

	vec_append(dst, buffer, size);

	tjFree(buffer);
	tjDestroy(handle);

	return 0;
}

static void tight_png_write(png_structp png_ptr, png_bytep data, png_size_t size)
{
	struct vec* out = png_get_io_ptr(png_ptr);
	assert(out);

	vec_append(out, data, size);
}

static void tight_png_flush(png_structp png_ptr)
{
}

int tight_encode_box_png(struct vec* dst, struct nvnc_client* client,
                         const struct nvnc_fb* fb, uint32_t x, uint32_t y,
                         uint32_t stride, uint32_t width, uint32_t height)
{
	png_structp png_ptr =
		png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

	png_infop info_ptr = png_create_info_struct(png_ptr);

	if (setjmp(png_jmpbuf(png_ptr)))
		return -1;

	int colour_type = PNG_COLOR_TYPE_RGB;

	struct vec pngout;
	vec_init(&pngout, 4096);

	png_set_write_fn(png_ptr, &pngout, tight_png_write, tight_png_flush);

	png_set_IHDR(png_ptr, info_ptr, width, height, 8, colour_type,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		     PNG_COMPRESSION_TYPE_DEFAULT);

	png_write_info(png_ptr, info_ptr);

	png_byte* buffer = malloc(3 * width * sizeof(*buffer));

	struct rfb_pixel_format dst_fmt = { 0 };
	rfb_pixfmt_from_fourcc(&dst_fmt, DRM_FORMAT_RGB888 | DRM_FORMAT_BIG_ENDIAN);

	struct rfb_pixel_format src_fmt = { 0 };
	rfb_pixfmt_from_fourcc(&dst_fmt, fb->fourcc_format);

	for (uint32_t r = 0; r < height; ++r) {
		uint32_t* row = (uint32_t*)fb->addr + x + (y + r) * stride;

		pixel32_to_cpixel(buffer, &dst_fmt, row, &src_fmt, 3, width);

		png_write_row(png_ptr, buffer);
	}

	png_write_end(png_ptr, info_ptr);
	png_write_flush(png_ptr);

	free(buffer);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	vec_reserve(dst, 4096);
	vec_fast_append_8(dst, TIGHT_PNG);

	tight_encode_size(dst, pngout.len);

	vec_append(dst, pngout.data, pngout.len);

	vec_destroy(&pngout);
	return 0;
}

int tight_encode_box(struct vec* dst, struct nvnc_client* client,
                     const struct nvnc_fb* fb, uint32_t x, uint32_t y,
                     uint32_t stride, uint32_t width, uint32_t height)
{
	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_TIGHT_PNG),
		.x = htons(x),
		.y = htons(y),
		.width = htons(width),
		.height = htons(height),
	};

	vec_append(dst, &rect, sizeof(rect));

	return tight_encode_box_png(dst, client, fb, x, y, stride, width,
	                             height);
}

int tight_encode_frame(struct vec* dst, struct nvnc_client* client,
                       const struct nvnc_fb* fb, struct pixman_region16* region)
{
	int rc = -1;

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(region, &n_rects);
	if (n_rects > UINT16_MAX) {
		box = pixman_region_extents(region);
		n_rects = 1;
	}

	struct rfb_server_fb_update_msg head = {
	        .type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
	        .n_rects = htons(n_rects),
	};

	rc = vec_append(dst, &head, sizeof(head));
	if (rc < 0)
		return -1;

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int box_width = box[i].x2 - x;
		int box_height = box[i].y2 - y;

		rc = tight_encode_box(dst, client, fb, x, y,
		                      fb->width, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	return 0;
}
