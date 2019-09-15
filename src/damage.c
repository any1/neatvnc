#include "neatvnc.h"

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <pixman.h>
#include <string.h>
#include <sys/param.h>
#include <libdrm/drm_fourcc.h>
#include <uv.h>
#include <assert.h>

#define EXPORT __attribute__((visibility("default")))
#define ALIGN_DOWN(a, b) (((a) / (b)) * (b))

struct damage_check {
	uv_work_t work;
	const struct nvnc_fb *fb0;
	const struct nvnc_fb *fb1;
	int x_hint;
	int y_hint;
	int width_hint;
	int height_hint;
	nvnc_damage_fn on_done;
	struct pixman_region16 damage;
	void *userdata;
};

static bool fbs_are_compatible(const struct nvnc_fb *fb0,
			       const struct nvnc_fb *fb1)
{
	return fb0->fourcc_format == fb1->fourcc_format
	    && fb0->fourcc_modifier == fb1->fourcc_modifier
	    && fb0->nvnc_modifier == fb1->nvnc_modifier
	    && fb0->width == fb1->width
	    && fb0->height == fb1->height;
}

static inline bool are_tiles_equal(const uint32_t* a, const uint32_t* b,
				   int stride, int width, int height)
{
	for (int y = 0; y < height; ++y)
		if (memcmp(a + y * stride, b + y * stride, width * 4) != 0)
			return false;

	return true;
}

#define TILE_SIDE_LENGTH 32
int check_damage_linear(struct pixman_region16 *damage,
			const struct nvnc_fb *fb0, const struct nvnc_fb *fb1,
			int x_hint, int y_hint, int width_hint, int height_hint)
{
	uint32_t *b0 = fb0->addr;
	uint32_t *b1 = fb1->addr;

	int width = fb0->width;
	int height = fb0->height;

	assert(x_hint + width_hint <= width);
	assert(y_hint + height_hint <= height);

	int x_start = ALIGN_DOWN(x_hint, TILE_SIDE_LENGTH);
	int y_start = ALIGN_DOWN(y_hint, TILE_SIDE_LENGTH);
	
	for (int y = y_start; y < y_start + height_hint;
	     y += TILE_SIDE_LENGTH) {
		int tile_height = MIN(TILE_SIDE_LENGTH, height - y);

		for (int x = x_start; x < x_start + width_hint;
		     x += TILE_SIDE_LENGTH) {
			int tile_width = MIN(TILE_SIDE_LENGTH, width - x);

			if (are_tiles_equal(b0 + x + y * width,
					    b1 + x + y * width,
					    width, tile_width, tile_height))
				continue;

			pixman_region_union_rect(damage, damage, x, y,
						 tile_width, tile_height);
		}
	}

	return 0;
}
#undef TILE_SIDE_LENGTH

void do_damage_check_linear(uv_work_t *work)
{
	struct damage_check *check = (void*)work;

	check_damage_linear(&check->damage, check->fb0, check->fb1,
			    check->x_hint, check->y_hint,
			    check->width_hint, check->height_hint);
}

void on_damage_check_done_linear(uv_work_t *work, int status)
{
	(void)status;

	struct damage_check *check = (void*)work;

	check->on_done(&check->damage, check->userdata);

	pixman_region_fini(&check->damage);
	free(check);
}

int check_damage_linear_threaded(const struct nvnc_fb *fb0,
				 const struct nvnc_fb *fb1,
				 int x_hint, int y_hint,
				 int width_hint, int height_hint,
				 nvnc_damage_fn on_check_done,
				 void *userdata)
{
	struct damage_check *work = calloc(1, sizeof(*work));
	if (!work)
		return -1;

	work->on_done = on_check_done;
	work->userdata = userdata;
	work->fb0 = fb0;
	work->fb1 = fb1;
	work->x_hint = x_hint;
	work->y_hint = y_hint;
	work->width_hint = width_hint;
	work->height_hint = height_hint;
	pixman_region_init(&work->damage);

	/* TODO: Spread the work into more tasks */
	int rc = uv_queue_work(uv_default_loop(), &work->work,
			       do_damage_check_linear,
			       on_damage_check_done_linear);
	if (rc < 0)
		free(work);

	return rc;
}

EXPORT
int nvnc_check_damage(const struct nvnc_fb *fb0, const struct nvnc_fb *fb1,
		      int x_hint, int y_hint, int width_hint, int height_hint,
		      nvnc_damage_fn on_check_done, void *userdata)
{
	if (!fbs_are_compatible(fb0, fb1))
		return -1;

	switch (fb0->fourcc_modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return check_damage_linear_threaded(fb0, fb1, x_hint, y_hint,
						    width_hint, height_hint,
						    on_check_done, userdata);
	}

	return -1;
}
