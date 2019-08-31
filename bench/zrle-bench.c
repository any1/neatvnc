#include "zrle.h"
#include "rfb-proto.h"
#include "util.h"
#include "vec.h"
#include "neatvnc.h"

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

int read_png_file(struct nvnc_fb* fb, const char *filename);

int run_benchmark(const char *image)
{
	int rc = -1;

	struct nvnc_fb fb;
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

	uint64_t start_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);

	rc = zrle_encode_frame(&frame, &pixfmt, fb.addr, &pixfmt,
			       fb.width, fb.height, &region);

	uint64_t end_time = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	printf("Encoding %s took %"PRIu64" micro seconds\n", image,
	       end_time - start_time);

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
