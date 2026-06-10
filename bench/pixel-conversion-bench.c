/*
 * Copyright (c) 2026 Andri Yngvason
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

#include "pixels.h"
#include "frame.h"
#include "rfb-proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>
#include <libdrm/drm_fourcc.h>

#define WIDTH 1920
#define HEIGHT 1080

struct stopwatch {
	uint64_t cpu;
	uint64_t real;
};

static uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void stopwatch_start(struct stopwatch* self)
{
	self->real = gettime_us(CLOCK_MONOTONIC);
	self->cpu = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
}

static uint64_t stopwatch_cpu_us(const struct stopwatch* self)
{
	uint64_t cpu_stop = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	return cpu_stop - self->cpu;
}

static int run_permutation(uint32_t src_fmt, uint32_t dst_fmt)
{
	struct nvnc_frame* src = nvnc_frame_new(WIDTH, HEIGHT, src_fmt, WIDTH);
	if (!src) {
		fprintf(stderr, "Failed to allocate source frame\n");
		return -1;
	}

	uint8_t* src_addr = src->buffer->addr;
	int src_bpp = nvnc__pixel_size_from_fourcc(src_fmt);
	size_t src_size = (size_t)HEIGHT * WIDTH * src_bpp;

	for (size_t i = 0; i < src_size; ++i)
		src_addr[i] = (uint8_t)i;

	int dst_bpp = nvnc__pixel_size_from_fourcc(dst_fmt);
	uint32_t dst_stride = (uint32_t)WIDTH * dst_bpp;
	size_t dst_size = (size_t)HEIGHT * dst_stride;

	void* dst = malloc(dst_size);
	if (!dst) {
		fprintf(stderr, "Failed to allocate destination buffer\n");
		nvnc_frame_unref(src);
		return -1;
	}

	struct nvnc_pixel_format src_pixfmt, dst_pixfmt;
	if (nvnc_pixel_format_from_fourcc(&src_pixfmt, src_fmt) < 0 ||
			nvnc_pixel_format_from_fourcc(&dst_pixfmt,
				dst_fmt) < 0) {
		fprintf(stderr, "Unsupported pixel format\n");
		free(dst);
		nvnc_frame_unref(src);
		return -1;
	}

	struct stopwatch stopwatch;
	stopwatch_start(&stopwatch);

	nvnc_frame_copy_region(src, &(const struct nvnc_frame_copy_options){
		.buffer = dst,
		.format = &dst_pixfmt,
		.stride = WIDTH,
		.crop.width = WIDTH,
		.crop.height = HEIGHT,
	});

	uint64_t dt_cpu = stopwatch_cpu_us(&stopwatch);

	printf("%"PRIu64"\t%s\t%s\n", dt_cpu, drm_format_to_string(src_fmt),
			drm_format_to_string(dst_fmt));

	free(dst);
	nvnc_frame_unref(src);

	return 0;
}

int main(int argc, char* argv[])
{
	for (size_t i = 0; nvnc_supported_pixel_formats[i]; ++i)
		for (size_t j = 0; nvnc_supported_pixel_formats[j]; ++j)
			if (run_permutation(nvnc_supported_pixel_formats[i],
						nvnc_supported_pixel_formats[j]) < 0)
				return 1;

	return 0;
}
