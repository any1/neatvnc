#pragma once

struct vec;
struct nvnc_client;
struct nvnc_fb;
struct pixman_region16;

int tight_encode_frame(struct vec* dst, struct nvnc_client* client,
                       const struct nvnc_fb* fb,
                       struct pixman_region16* region);
