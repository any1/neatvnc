#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ME_BLOCK_SIZE 32

struct nvnc_fb;
struct pixman_region16;

struct motion_vector {
	bool valid;
	int x, y;
};

struct motion_estimator {
	uint32_t width;
	uint32_t height;
	struct nvnc_fb* last_frame;
	struct motion_vector* vectors;
	uint32_t vwidth;
	uint32_t vheight;
};

int motion_estimator_init(struct motion_estimator* self, uint32_t width,
		uint32_t height);
void motion_estimator_deinit(struct motion_estimator* self);

int motion_estimate(struct motion_estimator* self, struct nvnc_fb* frame,
		struct pixman_region16* damage);
