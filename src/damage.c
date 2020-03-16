#include "neatvnc.h"
#include "fb.h"

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <pixman.h>
#include <string.h>
#include <sys/param.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>
#include <assert.h>

#define EXPORT __attribute__((visibility("default")))
#define ALIGN_DOWN(a, b) (((a) / (b)) * (b))

struct damage_check {
	struct aml_work* work;
	struct nvnc_fb* fb0;
	struct nvnc_fb* fb1;
	int x_hint;
	int y_hint;
	int width_hint;
	int height_hint;
	nvnc_damage_fn on_done;
	struct pixman_region16 damage;
	void* userdata;
};

static bool fbs_are_compatible(const struct nvnc_fb* fb0,
                               const struct nvnc_fb* fb1)
{
	return fb0->fourcc_format == fb1->fourcc_format &&
	       fb0->width == fb1->width && fb0->height == fb1->height;
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
int check_damage_linear(struct pixman_region16* damage,
                        const struct nvnc_fb* fb0, const struct nvnc_fb* fb1,
                        int x_hint, int y_hint, int width_hint, int height_hint)
{
	uint32_t* b0 = fb0->addr;
	uint32_t* b1 = fb1->addr;

	int width = fb0->width;
	int height = fb0->height;

	assert(x_hint + width_hint <= width);
	assert(y_hint + height_hint <= height);

	int x_start = ALIGN_DOWN(x_hint, TILE_SIDE_LENGTH);
	int y_start = ALIGN_DOWN(y_hint, TILE_SIDE_LENGTH);

	width_hint += x_hint - x_start;
	height_hint += y_hint - y_start;

	for (int y = y_start; y < y_start + height_hint; y += TILE_SIDE_LENGTH) {
		int tile_height = MIN(TILE_SIDE_LENGTH, height - y);

		for (int x = x_start; x < x_start + width_hint;
		     x += TILE_SIDE_LENGTH) {
			int tile_width = MIN(TILE_SIDE_LENGTH, width - x);

			int offset = x + y * width;

			if (are_tiles_equal(b0 + offset, b1 + offset, width,
			                    tile_width, tile_height))
				continue;

			pixman_region_union_rect(damage, damage, x, y,
			                         tile_width, tile_height);
		}
	}

	return 0;
}
#undef TILE_SIDE_LENGTH

void do_damage_check_linear(void* work)
{
	struct damage_check* check = aml_get_userdata(work);

	check_damage_linear(&check->damage, check->fb0, check->fb1,
	                    check->x_hint, check->y_hint, check->width_hint,
	                    check->height_hint);
}

void on_damage_check_done_linear(void* work)
{
	struct damage_check* check = aml_get_userdata(work);

	check->on_done(&check->damage, check->userdata);

	nvnc_fb_unref(check->fb1);
	nvnc_fb_unref(check->fb0);

	pixman_region_fini(&check->damage);
	free(check);
}

int check_damage_linear_threaded(struct nvnc_fb* fb0, struct nvnc_fb* fb1,
                                 int x_hint, int y_hint,
                                 int width_hint, int height_hint,
                                 nvnc_damage_fn on_check_done, void* userdata)
{
	struct damage_check* work = calloc(1, sizeof(*work));
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
	struct aml_work* obj =
		aml_work_new(do_damage_check_linear, on_damage_check_done_linear,
		             work, free);
	if (!obj) {
		free(work);
		return -1;
	}

	int rc = aml_start(aml_get_default(), obj);
	aml_unref(obj);
	if (rc < 0)
		return -1;

	work->work = obj;

	nvnc_fb_ref(fb0);
	nvnc_fb_ref(fb1);

	return 0;
}

EXPORT
int nvnc_check_damage(struct nvnc_fb* fb0, struct nvnc_fb* fb1,
                      int x_hint, int y_hint, int width_hint, int height_hint,
                      nvnc_damage_fn on_check_done, void* userdata)
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
