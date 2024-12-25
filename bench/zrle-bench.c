/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
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

#include "enc/encoder.h"
#include "rfb-proto.h"
#include "vec.h"
#include "neatvnc.h"
#include "pixels.h"

#include <aml.h>
#include <stdio.h>
#include <stdlib.h>
#include <libdrm/drm_fourcc.h>
#include <pixman.h>
#include <time.h>
#include <inttypes.h>

static struct encoded_frame* encoded_frame;

static uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

#pragma GCC push_options
#pragma GCC optimize ("-O0")
static void memcpy_unoptimized(void* dst, const void* src, size_t len)
{
	memcpy(dst, src, len);
}
#pragma GCC pop_options

struct nvnc_fb* read_png_file(const char *filename);

static void on_encoding_done(struct encoder* enc, struct encoded_frame* frame)
{
	encoded_frame = frame;
	encoded_frame_ref(frame);
	aml_exit(aml_get_default());
}

static int run_benchmark(const char *image)
{
	int rc = -1;

	struct nvnc_fb* fb = read_png_file(image);
	if (!fb)
		return -1;

	void *addr = nvnc_fb_get_addr(fb);
	int width = nvnc_fb_get_width(fb);
	int height = nvnc_fb_get_height(fb);
	int stride = nvnc_fb_get_stride(fb);

	struct rfb_pixel_format pixfmt;
	rfb_pixfmt_from_fourcc(&pixfmt, DRM_FORMAT_ARGB8888);

	struct pixman_region16 region;
	pixman_region_init(&region);

	pixman_region_union_rect(&region, &region, 0, 0, width, height);


	struct encoder* enc = encoder_new(RFB_ENCODING_ZRLE, width, height);
	assert(enc);

	enc->on_done = on_encoding_done;

	encoder_set_output_format(enc, &pixfmt);

	void *dummy = malloc(stride * height * 4);
	if (!dummy)
		goto failure;

	uint64_t start_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);

	memcpy_unoptimized(dummy, addr, stride * height * 4);

	uint64_t end_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	printf("memcpy baseline for %s took %"PRIu64" µs\n", image,
			end_time - start_time);

	free(dummy);

	start_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	rc = encoder_encode(enc, fb, &region);

	aml_run(aml_get_default());

	assert(encoded_frame);

	end_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	printf("Encoding %s took %"PRIu64" µs\n", image,
			end_time - start_time);

	double orig_size = stride * height * 4;
	double compressed_size = encoded_frame->buf.size;

	double reduction = (orig_size - compressed_size) / orig_size;
	printf("Size reduction: %.1f %%\n", reduction * 100.0);

	encoder_unref(enc);

	if (rc < 0)
		goto failure;

	rc = 0;
failure:
	pixman_region_fini(&region);
	encoded_frame_unref(encoded_frame);
	nvnc_fb_unref(fb);
	return 0;
}

int main(int argc, char *argv[])
{
	int rc = 0;

	char *image = argv[1];

	if (image)
		return run_benchmark(image) < 0 ? 1 :0;

	struct aml* aml = aml_new();
	aml_set_default(aml);

	aml_require_workers(aml, -1);

	rc |= run_benchmark("test-images/tv-test-card.png") < 0 ? 1 : 0;
	rc |= run_benchmark("test-images/mandrill.png") < 0 ? 1 : 0;

	aml_unref(aml);

	return rc;
}
