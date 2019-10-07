/*
 * Copyright (c) 2019 Andri Yngvason
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

#include "zrle.h"
#include "rfb-proto.h"
#include "util.h"
#include "vec.h"
#include "neatvnc.h"
#include "miniz.h"

#include <stdlib.h>
#include <libdrm/drm_fourcc.h>
#include <pixman.h>
#include <time.h>
#include <inttypes.h>

uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

#pragma push_options
#pragma GCC optimize ("-O0")
void memcpy_unoptimized(void* dst, const void* src, size_t len)
{
	memcpy(dst, src, len);
}
#pragma pop_options

struct nvnc_fb* read_png_file(const char *filename);

int run_benchmark(const char *image)
{
	int rc = -1;

	struct nvnc_fb* fb = read_png_file(image);
	if (!fb)
		return -1;

	void *addr = nvnc_fb_get_addr(fb);
	int width = nvnc_fb_get_width(fb);
	int height = nvnc_fb_get_height(fb);

	struct rfb_pixel_format pixfmt;
	rfb_pixfmt_from_fourcc(&pixfmt, DRM_FORMAT_ARGB8888);

	struct pixman_region16 region;
	pixman_region_init(&region);

	pixman_region_union_rect(&region, &region, 0, 0, width, height);

	struct vec frame;
	vec_init(&frame, width * height * 3 / 2);

	z_stream zs = { 0 };

	deflateInit(&zs, Z_DEFAULT_COMPRESSION);

	void *dummy = malloc(width * height * 4);
	if (!dummy)
		goto failure;

	uint64_t start_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);

	memcpy_unoptimized(dummy, addr, width * height * 4);

	uint64_t end_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	printf("memcpy baseline for %s took %"PRIu64" micro seconds\n", image,
	       end_time - start_time);

	free(dummy);

	start_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	rc = zrle_encode_frame(&zs, &frame, &pixfmt, fb, &pixfmt, &region);

	end_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	printf("Encoding %s took %"PRIu64" micro seconds\n", image,
	       end_time - start_time);

	deflateEnd(&zs);

	if (rc < 0)
		goto failure;

	rc = 0;
failure:
	pixman_region_fini(&region);
	vec_destroy(&frame);
	nvnc_fb_unref(fb);
	return 0;
}

int main(int argc, char *argv[])
{
	int rc = 0;

	char *image = argv[1];

	if (image)
		return run_benchmark(image) < 0 ? 1 :0;

	rc |= run_benchmark("test-images/tv-test-card.png") < 0 ? 1 : 0;
	rc |= run_benchmark("test-images/lena-soderberg.png") < 0 ? 1 : 0;

	return rc;
}
