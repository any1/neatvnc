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

#include "neatvnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aml.h>
#include <assert.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

struct draw {
	struct nvnc_fb* fb;
};

void on_pointer_event(struct nvnc_client* client, uint16_t x, uint16_t y,
                      enum nvnc_button_mask buttons)
{
	if (!(buttons & NVNC_BUTTON_LEFT))
		return;

	struct nvnc* server = nvnc_get_server(client);
	assert(server);

	struct draw* draw = nvnc_get_userdata(server);
	assert(draw);

	uint32_t* image = nvnc_fb_get_addr(draw->fb);
	int width = nvnc_fb_get_width(draw->fb);
	int height = nvnc_fb_get_height(draw->fb);

	image[x + y * width] = 0;

	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, width, height);
	pixman_region_intersect_rect(&region, &region, x, y, 1, 1);
	nvnc_feed_frame(server, draw->fb, &region);
	pixman_region_fini(&region);
}

int main(int argc, char* argv[])
{
	struct draw draw;

	int width = 500, height = 500;
	uint32_t format = DRM_FORMAT_RGBX8888;
	draw.fb = nvnc_fb_new(width, height, format);
	assert(draw.fb);

	void* addr = nvnc_fb_get_addr(draw.fb);

	memset(addr, 0xff, width * height * 4);

	struct aml* aml = aml_new(NULL, 0);
	aml_set_default(aml);

	struct nvnc* server = nvnc_open("127.0.0.1", 5900);

	nvnc_set_dimensions(server, width, height, format);
	nvnc_set_name(server, "Draw");
	nvnc_set_pointer_fn(server, on_pointer_event);
	nvnc_set_userdata(server, &draw);

	aml_run(aml);

	nvnc_close(server);
}
