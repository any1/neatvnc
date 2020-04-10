/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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

#include "neatvnc.h"
#include "rfb-proto.h"
#include "vec.h"
#include "fb.h"
#include "tight.h"
#include "common.h"
#include "pixels.h"
#include "logging.h"
#include "type-macros.h"

#include <pixman.h>
#include <turbojpeg.h>
#include <stdlib.h>
#include <sys/param.h>
#include <libdrm/drm_fourcc.h>
#include <zlib.h>

#define TIGHT_FILL 0x80
#define TIGHT_JPEG 0x90
#define TIGHT_PNG 0xA0
#define TIGHT_BASIC 0x00

#define TIGHT_MAX_WIDTH 2048

enum tight_quality tight_get_quality(struct tight_encoder* self)
{
	struct nvnc_client* client =
		container_of(self, struct nvnc_client, tight_encoder);

	/* Note: The standard specifies that 16 is OK too, but we can only
	 * handle 32
	 */
	if (client->pixfmt.bits_per_pixel != 32)
		return TIGHT_QUALITY_LOSSLESS;

	for (size_t i = 0; i < client->n_encodings; ++i)
		switch (client->encodings[i]) {
		case RFB_ENCODING_JPEG_HIGHQ: return TIGHT_QUALITY_HIGH;
		case RFB_ENCODING_JPEG_LOWQ: return TIGHT_QUALITY_LOW;
		default:;
		}

	return TIGHT_QUALITY_LOSSLESS;
}

int tight_init_zstream(z_stream* zx)
{
	int rc = deflateInit2(zx,
	                      /* compression level: */ 1,
	                      /*            method: */ Z_DEFLATED,
	                      /*       window bits: */ 15,
	                      /*         mem level: */ 9,
	                      /*          strategy: */ Z_DEFAULT_STRATEGY);
	return rc == Z_OK ? 0 : -1;
}

int tight_encoder_init(struct tight_encoder* self)
{
	// TODO: Implement more stream channels
	return tight_init_zstream(&self->zs[0]);
}

void tight_encoder_destroy(struct tight_encoder* self)
{
	deflateEnd(&self->zs[0]);
}

static int calc_bytes_per_cpixel(const struct rfb_pixel_format* fmt)
{
	return fmt->bits_per_pixel == 32 ? fmt->depth / 8
	                                 : fmt->bits_per_pixel / 8;
}

enum TJPF get_jpeg_pixfmt(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		return TJPF_XBGR;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		return TJPF_XRGB;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return TJPF_BGRX;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return TJPF_RGBX;
	}

	return TJPF_UNKNOWN;
}

static void tight_encode_size(struct vec* dst, size_t size)
{
	vec_fast_append_8(dst, (size & 0x7f) | ((size >= 128) << 7));
	if (size >= 128)
		vec_fast_append_8(dst, ((size >> 7) & 0x7f) | ((size >= 16384) << 7));
	if (size >= 16384)
		vec_fast_append_8(dst, (size >> 14) & 0xff);
}

int tight_encode_box_jpeg(struct tight_encoder* self, struct vec* dst,
                          const struct nvnc_fb* fb, uint32_t x, uint32_t y,
                          uint32_t stride, uint32_t width, uint32_t height)
{

	unsigned char* buffer = NULL;
	size_t size = 0;

	int quality; /* 1 - 100 */

	switch (self->quality) {
	case TIGHT_QUALITY_HIGH: quality = 66; break;
	case TIGHT_QUALITY_LOW: quality = 33; break;
	default: abort();
	}

	enum TJPF tjfmt = get_jpeg_pixfmt(fb->fourcc_format);
	if (tjfmt == TJPF_UNKNOWN)
		return -1;

	vec_reserve(dst, 4096);

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_TIGHT),
		.x = htons(x),
		.y = htons(y),
		.width = htons(width),
		.height = htons(height),
	};

	vec_append(dst, &rect, sizeof(rect));

	tjhandle handle = tjInitCompress();
	if (!handle)
		return -1;

	void* img = (uint32_t*)fb->addr + x + y * stride;

	int rc = -1;
	rc = tjCompress2(handle, img, width, stride * 4, height, tjfmt, &buffer,
	                 &size, TJSAMP_422, quality, TJFLAG_FASTDCT);
	if (rc < 0) {
		log_error("Failed to encode tight JPEG box: %s\n", tjGetErrorStr());
		goto compress_failure;
	}

	vec_fast_append_8(dst, TIGHT_JPEG);

	tight_encode_size(dst, size);

	vec_append(dst, buffer, size);

	rc = 0;
	tjFree(buffer);
compress_failure:
	tjDestroy(handle);

	return rc;
}

int tight_deflate(struct vec* dst, void* src, size_t len, z_stream* zs, bool flush)
{
	zs->next_in = src;
	zs->avail_in = len;

	do {
		if (dst->len == dst->cap && vec_reserve(dst, dst->cap * 2) < 0)
			return -1;

		zs->next_out = ((Bytef*)dst->data) + dst->len;
		zs->avail_out = dst->cap - dst->len;

		int r = deflate(zs, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
		if (r == Z_STREAM_ERROR)
			return -1;

		dst->len = zs->next_out - (Bytef*)dst->data;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}

int tight_encode_box_basic(struct tight_encoder* self, struct vec* dst,
                           const struct nvnc_fb* fb,
                           const struct rfb_pixel_format* src_fmt,
                           uint32_t x, uint32_t y_start,
                           uint32_t stride, uint32_t width, uint32_t height)
{
	struct nvnc_client* client =
		container_of(self, struct nvnc_client, tight_encoder);

	vec_reserve(dst, 4096);

	int bytes_per_cpixel = calc_bytes_per_cpixel(&client->pixfmt);
	uint8_t* row = malloc(bytes_per_cpixel * width);
	if (!row)
		return -1;

	struct vec buffer;
	if (vec_init(&buffer, 4096) < 0)
		goto buffer_failure;

	struct rfb_server_fb_rect rect = {
		.encoding = htonl(RFB_ENCODING_TIGHT),
		.x = htons(x),
		.y = htons(y_start),
		.width = htons(width),
		.height = htons(height),
	};

	vec_append(dst, &rect, sizeof(rect));

	vec_fast_append_8(dst, TIGHT_BASIC);

	struct rfb_pixel_format cfmt = { 0 };
	if (bytes_per_cpixel == 3)
		rfb_pixfmt_from_fourcc(&cfmt, DRM_FORMAT_RGBX8888 | DRM_FORMAT_BIG_ENDIAN);
	else
		memcpy(&cfmt, &client->pixfmt, sizeof(cfmt));

	if (width * height * bytes_per_cpixel < 12) {
		for (uint32_t y = y_start; y < y_start + height; ++y) {
			void* img = (uint32_t*)fb->addr + x + y * stride;
			pixel32_to_cpixel(row, &cfmt, img, src_fmt,
			                  bytes_per_cpixel, width);
			vec_append(&buffer, row, width * bytes_per_cpixel);
		}
	} else {
		for (uint32_t y = y_start; y < y_start + height; ++y) {
			void* img = (uint32_t*)fb->addr + x + y * stride;
			pixel32_to_cpixel(row, &cfmt, img, src_fmt,
			                  bytes_per_cpixel, width);
			tight_deflate(&buffer, row, bytes_per_cpixel * width,
					&self->zs[0], y == y_start + height - 1);
		}

		tight_encode_size(dst, buffer.len);
	}

	vec_append(dst, buffer.data, buffer.len);

	vec_destroy(&buffer);
	free(row);
	return 0;

buffer_failure:
	free(row);
	return -1;
}

int tight_encode_box(struct tight_encoder* self, struct vec* dst,
                     const struct nvnc_fb* fb,
                     const struct rfb_pixel_format* src_fmt,
                     uint32_t x, uint32_t y,
                     uint32_t stride, uint32_t width, uint32_t height)
{
	switch (self->quality) {
	case TIGHT_QUALITY_LOSSLESS:
		return tight_encode_box_basic(self, dst, fb, src_fmt, x, y,
		                              stride, width, height);
	case TIGHT_QUALITY_HIGH:
	case TIGHT_QUALITY_LOW:
		return tight_encode_box_jpeg(self, dst, fb, x, y, stride,
		                             width, height);
	case TIGHT_QUALITY_UNSPEC:;
	}

	abort();
	return -1;
}

int tight_encode_frame(struct tight_encoder* self, struct vec* dst,
                       const struct nvnc_fb* fb,
                       const struct rfb_pixel_format* src_fmt,
                       struct pixman_region16* region)
{
	self->quality = tight_get_quality(self);

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

		rc = tight_encode_box(self, dst, fb, src_fmt, x, y,
		                      fb->width, box_width, box_height);
		if (rc < 0)
			return -1;
	}

	return 0;
}
