#pragma once

struct nvnc_fb;
struct rfb_pixel_format;
struct pixman_region16;
struct vec;

int raw_encode_frame(struct vec* dst, const struct rfb_pixel_format* dst_fmt,
                     const struct nvnc_fb* src,
                     const struct rfb_pixel_format* src_fmt,
                     struct pixman_region16* region);
