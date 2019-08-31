#include "neatvnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <assert.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

struct draw {
	struct nvnc_fb fb;
};

void on_fb_req(struct nvnc *nvnc, bool incremental, uint16_t x, uint16_t y,
	       uint16_t width, uint16_t height)
{
	if (incremental)
		return;

	struct draw *draw = nvnc_get_userdata(nvnc);
	assert(draw);

	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, draw->fb.width,
				draw->fb.height);
	nvnc_update_fb(nvnc, &draw->fb, &region);
	pixman_region_fini(&region);
}

void on_pointer_event(struct nvnc *nvnc, uint16_t x, uint16_t y,
		      enum nvnc_button_mask buttons)
{
	if (!(buttons & NVNC_BUTTON_LEFT))
		return;

	struct draw *draw = nvnc_get_userdata(nvnc);
	assert(draw);

	uint32_t *image = draw->fb.addr;

	image[x + y * draw->fb.width] = 0;

	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, draw->fb.width,
				draw->fb.height);
//	pixman_region_init_rect(&region, x, y, 1, 1);
	nvnc_update_fb(nvnc, &draw->fb, &region);
	pixman_region_fini(&region);
}

int main(int argc, char *argv[])
{
	struct draw draw; 

	draw.fb.width = 500;
	draw.fb.height = 500;
	draw.fb.size = draw.fb.width * draw.fb.height * 4;
	draw.fb.fourcc_format = DRM_FORMAT_RGBX8888;
	draw.fb.fourcc_modifier = DRM_FORMAT_MOD_LINEAR;

	draw.fb.addr = malloc(draw.fb.size);
	assert(draw.fb.addr);

	memset(draw.fb.addr, 0xff, draw.fb.size);

	struct nvnc *server = nvnc_open("127.0.0.1", 5900);

	nvnc_set_dimensions(server, draw.fb.width, draw.fb.height,
			    draw.fb.fourcc_format);
	nvnc_set_name(server, "Draw");
	nvnc_set_fb_req_fn(server, on_fb_req);
	nvnc_set_pointer_fn(server, on_pointer_event);
	nvnc_set_userdata(server, &draw);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	nvnc_close(server);
}
