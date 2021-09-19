#pragma once

#include <stdint.h>

struct nvnc_fb;
struct nvnc_fb_pool;
struct pixman_region16;

struct resampler {
	struct nvnc_fb_pool *pool;
	void (*on_done)(struct resampler*, struct nvnc_fb*,
			struct pixman_region16* damage);
};

int resampler_init(struct resampler*);
void resampler_destroy(struct resampler*);

int resampler_feed(struct resampler*, struct nvnc_fb* fb,
		struct pixman_region16* damage);
