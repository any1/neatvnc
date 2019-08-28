//#pragma once

#include <stdint.h>
#include <stdbool.h>

struct nvnc;
struct pixman_region16;

enum nvnc_button_mask {
        NVNC_BUTTON_LEFT = 1 << 0,
        NVNC_BUTTON_MIDDLE = 1 << 1,
        NVNC_BUTTON_RIGHT = 1 << 2,
        NVNC_SCROLL_UP = 1 << 3,
        NVNC_SCROLL_DOWN = 1 << 4,
};

struct nvnc_fb {
        void *addr;
        uint32_t size;
        uint16_t width;
        uint16_t height;
        uint32_t fourcc_format;
        uint64_t fourcc_modifier;
};

typedef void (*nvnc_key_fn)(struct nvnc*, uint32_t keysym, bool is_pressed);
typedef void (*nvnc_pointer_fn)(struct nvnc*, uint16_t x, uint16_t y,
                                enum nvnc_button_mask);
typedef void (*nvnc_fb_req_fn)(struct nvnc*, bool is_incremental,
                               uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height);

struct nvnc *nvnc_open(const char *addr, uint16_t port);
void nvnc_close(struct nvnc *self);

void nvnc_set_userdata(struct nvnc *self, void* userdata);
void* nvnc_get_userdata(struct nvnc *self);

void nvnc_set_key_fn(struct nvnc *self, nvnc_key_fn);
void nvnc_set_pointer_fn(struct nvnc *self, nvnc_pointer_fn);
void nvnc_set_fb_req_fn(struct nvnc *self, nvnc_fb_req_fn);

int nvnc_update_fb(struct nvnc *self, const struct nvnc_fb* fb,
                   const struct pixman_region16* region);
