#include "zrle.h"
#include "rfb-proto.h"
#include "util.h"
#include "vec.h"

#include <stdlib.h>
#include <libdrm/drm_fourcc.h>
#include <pixman.h>

int read_png_file(struct vnc_framebuffer* fb, const char *filename);

int run_benchmark(const char *image)
{
	int rc = -1;

	struct vnc_framebuffer fb;
	rc = read_png_file(&fb, image);
	if (rc < 0)
		return -1;

	struct rfb_pixel_format pixfmt;
	rfb_pixfmt_from_fourcc(&pixfmt, DRM_FORMAT_ARGB8888);

	struct pixman_region16 region;
	pixman_region_init(&region);

	pixman_region_union_rect(&region, &region, 0, 0, fb.width, fb.height);

	struct vec frame;
	vec_init(&frame, fb.width * fb.height * 3 / 2);

	rc = zrle_encode_frame(&frame, &pixfmt, fb.addr, &pixfmt,
			       fb.width, fb.height, &region);

	if (rc < 0)
		goto failure;

	rc = 0;
failure:
	pixman_region_fini(&region);
	vec_destroy(&frame);
	free(fb.addr);
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
