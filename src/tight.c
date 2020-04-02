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
#include "logging.h"

#include <pixman.h>
#include <turbojpeg.h>
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
		vec_fast_append_8(dst, (size >> 14) & 0x7f);
}

int tight_encode_box(struct vec* dst, struct nvnc_client* client,
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
