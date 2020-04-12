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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>
#include <aml.h>
#include <signal.h>
#include <assert.h>
#include <pixman.h>
#include <sys/param.h>
#include <libdrm/drm_fourcc.h>

#define MAX_COORD 128

struct coord {
	int x, y;
};

struct draw {
	struct nvnc_fb* fb;
	struct coord coord[MAX_COORD];
	int index;
};

int coord_distance_between(struct coord a, struct coord b)
{
	float x = abs(a.x - b.x);
	float y = abs(a.y - b.y);
	return round(sqrt(x * x + y * y));
}

void draw_dot(struct nvnc* nvnc, struct nvnc_fb* fb, struct coord coord,
              int radius, uint32_t colour)
{
	uint32_t* image = nvnc_fb_get_addr(fb);
	int width = nvnc_fb_get_width(fb);
	int height = nvnc_fb_get_height(fb);

	struct coord start, stop;

	start.x = MAX(0, coord.x - radius);
	start.y = MAX(0, coord.y - radius);
	stop.x = MIN(width, coord.x + radius);
	stop.y = MIN(height, coord.y + radius);

	struct pixman_region16 region;
	pixman_region_init_rect(&region, start.x, start.y,
	                        stop.x - start.x, stop.y - start.y);
	nvnc_damage_region(nvnc, &region);
	pixman_region_fini(&region);

	/* The brute force method. ;) */
	for (int y = start.y; y < stop.y; ++y)
		for (int x = start.x; x < stop.x; ++x) {
			struct coord point = { .x = x, .y = y };
			if (coord_distance_between(point, coord) <= radius)
				image[x + y * width] = colour;
		}
}

void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
                      enum nvnc_button_mask buttons)
{
	if (!(buttons & NVNC_BUTTON_LEFT))
		return;

	struct nvnc* server = nvnc_get_server(client);
	assert(server);

	struct draw* draw = nvnc_get_userdata(server);
	assert(draw);

	int width = nvnc_fb_get_width(draw->fb);
	int height = nvnc_fb_get_height(draw->fb);

	if (draw->index >= MAX_COORD)
		return;

	draw->coord[draw->index].x = x;
	draw->coord[draw->index].y = y;
	draw->index++;

	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, width, height);
	pixman_region_intersect_rect(&region, &region, x, y, 1, 1);
	nvnc_damage_region(server, &region);
	pixman_region_fini(&region);
}

void on_render(struct nvnc* server, struct nvnc_fb* fb)
{
	struct draw* draw = nvnc_get_userdata(server);
	assert(draw);

	for (int i = 0; i < draw->index; ++i)
		draw_dot(server, fb, draw->coord[i], 16, 0);

	draw->index = 0;
}

void on_sigint()
{
	aml_exit(aml_get_default());
}

int main(int argc, char* argv[])
{
	struct draw draw = { 0 };

	int width = 500, height = 500;
	uint32_t format = DRM_FORMAT_RGBX8888;
	draw.fb = nvnc_fb_new(width, height, format);
	assert(draw.fb);

	void* addr = nvnc_fb_get_addr(draw.fb);

	memset(addr, 0xff, width * height * 4);

	struct aml* aml = aml_new(NULL, 0);
	aml_set_default(aml);

	struct nvnc* server = nvnc_open("127.0.0.1", 5900);

	nvnc_set_name(server, "Draw");
	nvnc_set_pointer_fn(server, on_pointer_event);
	nvnc_set_userdata(server, &draw);
	nvnc_set_buffer(server, draw.fb);
	nvnc_set_render_fn(server, on_render);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml_get_default(), sig);
	aml_unref(sig);

	aml_run(aml);

	nvnc_close(server);
	nvnc_fb_unref(draw.fb);
	aml_unref(aml);
}
