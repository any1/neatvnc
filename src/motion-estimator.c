#include "motion-estimator.h"
#include "fb.h"

#include <stdlib.h>
#include <assert.h>
#include <sys/param.h>
#include <tgmath.h>

// TODO: REMOVE
#include <stdio.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

int motion_estimator_init(struct motion_estimator* self, uint32_t width,
		uint32_t height)
{
	self->width = width;
	self->height = height;
	self->vwidth = UDIV_UP(width, ME_BLOCK_SIZE);
	self->vheight = UDIV_UP(height, ME_BLOCK_SIZE);
	self->vectors = calloc(self->vwidth * self->vheight,
			sizeof(*self->vectors));
	return self->vectors ? 0 : -1;
}

void motion_estimator_deinit(struct motion_estimator* self)
{
	free(self->vectors);
}

// TODO: This assumes that pixels are 32 bit RGB
static int compare_pixels(uint32_t a, uint32_t b)
{
	const uint8_t* ca = (const uint8_t*)&a;
	const uint8_t* cb = (const uint8_t*)&b;
	return abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
		+ abs(ca[3] - cb[3]);
}

static int compare_block(struct motion_estimator* self, struct nvnc_fb* last_frame,
		struct nvnc_fb* frame, uint32_t vx, uint32_t vy,
		uint32_t tx, uint32_t ty)
{
	uint32_t x_start = vx * ME_BLOCK_SIZE;
	uint32_t y_start = vy * ME_BLOCK_SIZE;
	uint32_t x_stop = MIN(self->width, (vx + 1) * ME_BLOCK_SIZE);
	uint32_t width = x_stop - x_start;
	uint32_t y_stop = MIN(self->height, (vy + 1) * ME_BLOCK_SIZE);
	uint32_t height = y_stop - y_start;

	uint32_t sum = 0;

	uint32_t stride = frame->stride;
	uint32_t* last = last_frame->addr;
	uint32_t* current = frame->addr;

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			uint32_t target_pixel = current[tx + x + (ty + y) * stride];
			uint32_t block_pixel =
				last[x_start + x + (y_start + y) * stride];
			sum += compare_pixels(target_pixel, block_pixel);
		}
	}

	return sum;
}

static bool box_overlaps_region(struct pixman_region16* region, uint32_t x1,
		uint32_t y1, uint32_t x2, uint32_t y2)
{
	struct pixman_box16 box = {
		.x1 = x1,
		.y1 = y1,
		.x2 = x2,
		.y2 = y2,
	};
	pixman_region_overlap_t overlap
		= pixman_region_contains_rectangle(region, &box);
	return overlap != PIXMAN_REGION_OUT;
}

static void find_block(struct motion_estimator* self,
		struct nvnc_fb* last_frame, struct nvnc_fb* frame, uint32_t vx,
		uint32_t vy, struct pixman_region16* damage)
{
	struct motion_vector* vector = self->vectors + vx + (vy * self->vwidth);

	if (!box_overlaps_region(damage,
				vx * ME_BLOCK_SIZE,
				vy * ME_BLOCK_SIZE,
				(vx + 1) * ME_BLOCK_SIZE,
				(vy + 1) * ME_BLOCK_SIZE)) {
		vector->valid = false;
		return;
	}

	uint16_t width = frame->width;
	uint16_t height = frame->height;

	int search_distance = ME_BLOCK_SIZE * 2;

	int block_x = vx * ME_BLOCK_SIZE;
	int block_y = vy * ME_BLOCK_SIZE;

	struct pixman_region16 search_region;
	pixman_region_init_rect(&search_region,
			MAX(0, block_x - search_distance / 2),
			MAX(0, block_y - search_distance / 2),
			search_distance, search_distance);

	pixman_region_intersect(&search_region, &search_region, damage);

	int n_rects = 0;
	struct pixman_box16* rects =
		pixman_region_rectangles(&search_region, &n_rects);
	for (int i = 0; i < n_rects; ++i) {
		struct pixman_box16* rect = &rects[i];
		uint32_t x1 = rect->x1;
		uint32_t y1 = rect->y1;
		uint32_t x2 = MIN(width - ME_BLOCK_SIZE, rect->x2);
		uint32_t y2 = MIN(height - ME_BLOCK_SIZE, rect->y2);

		for (uint32_t y = y1; y < y2; ++y) {
			for (uint32_t x = x1; x < x2; ++x) {
				if (x == vx * ME_BLOCK_SIZE &&
						y == vy * ME_BLOCK_SIZE)
					continue;

				uint32_t sad = compare_block(self, last_frame,
						frame, vx, vy, x, y);
				if (sad == 0) {
					vector->valid = true;
					vector->x = x;
					vector->y = y;
					break;
				}
			}
		}
	}

	pixman_region_fini(&search_region);
}

int motion_estimate(struct motion_estimator* self, struct nvnc_fb* frame,
		struct pixman_region16* damage)
{
	assert(self->width == (uint32_t)frame->width &&
			self->height == (uint32_t)frame->height);

	struct nvnc_fb* last_frame = self->last_frame;
	nvnc_fb_unref(self->last_frame);
	self->last_frame = frame;
	nvnc_fb_ref(self->last_frame);
	if (!last_frame)
		return -1;

	nvnc_fb_map(last_frame);
	nvnc_fb_map(frame);

	for (uint32_t vy = 0; vy < self->vheight; ++vy) {
		for (uint32_t vx = 0; vx < self->vwidth; ++vx) {
			find_block(self, last_frame, frame, vx, vy, damage);
		}
	}

	nvnc_fb_unmap(frame);
	nvnc_fb_unmap(last_frame);

	return 0;
}
