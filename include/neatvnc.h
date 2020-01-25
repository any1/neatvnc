/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct nvnc;
struct nvnc_client;
struct nvnc_fb;
struct pixman_region16;

enum nvnc_button_mask {
	NVNC_BUTTON_LEFT = 1 << 0,
	NVNC_BUTTON_MIDDLE = 1 << 1,
	NVNC_BUTTON_RIGHT = 1 << 2,
	NVNC_SCROLL_UP = 1 << 3,
	NVNC_SCROLL_DOWN = 1 << 4,
};

typedef void (*nvnc_key_fn)(struct nvnc_client*, uint32_t keysym,
                            bool is_pressed);
typedef void (*nvnc_pointer_fn)(struct nvnc_client*, uint16_t x, uint16_t y,
                                enum nvnc_button_mask);
typedef void (*nvnc_fb_req_fn)(struct nvnc_client*, bool is_incremental,
                               uint16_t x, uint16_t y, uint16_t width,
                               uint16_t height);
typedef void (*nvnc_client_fn)(struct nvnc_client*);
typedef void (*nvnc_damage_fn)(struct pixman_region16* damage, void* userdata);
typedef bool (*nvnc_auth_fn)(const char* username, const char* password,
                             void* userdata);

struct nvnc* nvnc_open(const char* addr, uint16_t port);
void nvnc_close(struct nvnc* self);

void nvnc_set_userdata(void* self, void* userdata);
void* nvnc_get_userdata(const void* self);

struct nvnc* nvnc_get_server(const struct nvnc_client* client);

void nvnc_set_dimensions(struct nvnc* self, uint16_t width, uint16_t height,
                         uint32_t fourcc_format);

void nvnc_set_name(struct nvnc* self, const char* name);

void nvnc_set_key_fn(struct nvnc* self, nvnc_key_fn);
void nvnc_set_pointer_fn(struct nvnc* self, nvnc_pointer_fn);
void nvnc_set_fb_req_fn(struct nvnc* self, nvnc_fb_req_fn);
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn);
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn);

bool nvnc_has_auth(void);
int nvnc_enable_auth(struct nvnc* self, const char* privkey_path,
                     const char* cert_path, nvnc_auth_fn, void* userdata);

struct nvnc_fb* nvnc_fb_new(uint16_t width, uint16_t height,
                            uint32_t fourcc_format);

void nvnc_fb_ref(struct nvnc_fb* fb);
void nvnc_fb_unref(struct nvnc_fb* fb);

void* nvnc_fb_get_addr(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb);
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb);

/*
 * Feed a new frame to the server. The damaged region is sent to clients
 * immediately.
 */
int nvnc_feed_frame(struct nvnc* self, struct nvnc_fb* fb,
                    const struct pixman_region16* damage);

/*
 * Find the regions that differ between fb0 and fb1. Regions outside the hinted
 * rectangle region are not guaranteed to be checked.
 *
 * This is a utility function that may be used to reduce network traffic.
 */
int nvnc_check_damage(const struct nvnc_fb* fb0, const struct nvnc_fb* fb1,
                      int x_hint, int y_hint, int width_hint, int height_hint,
                      nvnc_damage_fn on_check_done, void* userdata);
