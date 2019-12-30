#pragma once

#include <stdint.h>
#include <unistd.h>

struct nvnc_fb;
struct rfb_pixel_format;
struct pixman_region16;
struct vec;

int tight_encode_frame(struct vec* dst, const struct rfb_pixel_format* dst_fmt,
                       const struct nvnc_fb* src, uint32_t src_fmt,
                       struct pixman_region16* region);
