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

#include "damage-refinery.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <inttypes.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

#define TSL 32

static uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static uint32_t fourcc_from_pixel_size(int pixel_size)
{
	switch (pixel_size) {
	case 1: return DRM_FORMAT_RGB332;
	case 2: return DRM_FORMAT_RGB565;
	case 3: return DRM_FORMAT_RGB888;
	case 4: return DRM_FORMAT_XRGB8888;
	}
	return 0;
}

static void fill_frame(struct nvnc_frame* frame)
{
	uint8_t* pixels = frame->buffer->addr;
	size_t size = (size_t)frame->height * frame->stride *
		nvnc_frame_get_pixel_size(frame);

	for (size_t i = 0; i < size; ++i)
		pixels[i] = i;
}

static void update_frame(struct nvnc_frame* frame, uint32_t value)
{
	uint8_t* pixels = frame->buffer->addr;
	int pixel_size = nvnc_frame_get_pixel_size(frame);

	for (int y = 0; y < frame->height; y += TSL)
		for (int x = 0; x < frame->width; x += TSL)
			memcpy(pixels + (x + y * frame->stride) * pixel_size,
					&value, pixel_size);
}

static int run_benchmark(int width, int height, uint32_t fourcc,
		uint64_t duration_us)
{
	struct nvnc_frame* frame = nvnc_frame_new(width, height, fourcc, 0);
	if (!frame) {
		fprintf(stderr, "Failed to allocate frame\n");
		return -1;
	}

	printf("Allocated frame at address %p with stride %"PRIi32"\n",
			frame->buffer->addr, frame->stride);

	fill_frame(frame);

	struct damage_refinery refinery;
	if (damage_refinery_init(&refinery, width, height) < 0) {
		fprintf(stderr, "Failed to initialise damage refinery\n");
		nvnc_frame_unref(frame);
		return -1;
	}

	struct pixman_region16 hint;
	pixman_region_init_rect(&hint, 0, 0, width, height);

	uint64_t start = gettime_us(CLOCK_MONOTONIC);
	uint64_t deadline = start + duration_us;
	uint64_t now;
	int n_runs = 0;

	do {
		update_frame(frame, n_runs);

		struct pixman_region16 refined;
		pixman_region_init(&refined);
		damage_refine(&refinery, &refined, &hint, frame);
		pixman_region_fini(&refined);

		n_runs++;
		now = gettime_us(CLOCK_MONOTONIC);
	} while (now < deadline);

	uint64_t dt = now - start;

	printf("Number of runs: %d\n", n_runs);
	printf("Took %"PRIu64" µs\n", dt);
	printf("Runs per second: %.1f\n", n_runs * 1000000.0 / dt);

	pixman_region_fini(&hint);
	damage_refinery_destroy(&refinery);
	nvnc_frame_unref(frame);

	return 0;
}

static int usage(const char* self)
{
	fprintf(stderr, "Usage: %s [-d|--duration <seconds>] [-w|--width <pixels>] [-h|--height <pixels>] [-p|--pixel-size <1-4>]\n",
			self);
	return 1;
}

int main(int argc, char* argv[])
{
	static const struct option long_options[] = {
		{ "duration", required_argument, NULL, 'd' },
		{ "width", required_argument, NULL, 'w' },
		{ "height", required_argument, NULL, 'h' },
		{ "pixel-size", required_argument, NULL, 'p' },
		{ NULL, 0, NULL, 0 },
	};

	uint64_t duration_us = 1000000;
	int width = 1920;
	int height = 1080;
	int pixel_size = 4;

	int opt;
	while ((opt = getopt_long(argc, argv, "d:w:h:p:", long_options,
			NULL)) != -1) {
		switch (opt) {
		case 'd':
			duration_us = atof(optarg) * 1000000;
			break;
		case 'w':
			width = atoi(optarg);
			break;
		case 'h':
			height = atoi(optarg);
			break;
		case 'p':
			pixel_size = atoi(optarg);
			break;
		default:
			return usage(argv[0]);
		}
	}

	uint32_t fourcc = fourcc_from_pixel_size(pixel_size);
	if (!fourcc) {
		fprintf(stderr, "Unsupported pixel size: %d\n", pixel_size);
		return usage(argv[0]);
	}

	return run_benchmark(width, height, fourcc, duration_us) < 0 ? 1 : 0;
}
