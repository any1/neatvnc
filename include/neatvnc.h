/*
 * Copyright (c) 2019 - 2021 Andri Yngvason
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
struct nvnc_display;
struct nvnc_fb;
struct nvnc_fb_pool;
struct pixman_region16;
struct gbm_bo;

enum nvnc_button_mask {
	NVNC_BUTTON_LEFT = 1 << 0,
	NVNC_BUTTON_MIDDLE = 1 << 1,
	NVNC_BUTTON_RIGHT = 1 << 2,
	NVNC_SCROLL_UP = 1 << 3,
	NVNC_SCROLL_DOWN = 1 << 4,
};

enum nvnc_fb_type {
	NVNC_FB_UNSPEC = 0,
	NVNC_FB_SIMPLE,
	NVNC_FB_GBM_BO,
};

/* This is the same as wl_output_transform */
enum nvnc_transform {
	NVNC_TRANSFORM_NORMAL = 0,
	NVNC_TRANSFORM_90 = 1,
	NVNC_TRANSFORM_180 = 2,
	NVNC_TRANSFORM_270 = 3,
	NVNC_TRANSFORM_FLIPPED = 4,
	NVNC_TRANSFORM_FLIPPED_90 = 5,
	NVNC_TRANSFORM_FLIPPED_180 = 6,
	NVNC_TRANSFORM_FLIPPED_270 = 7,
};

typedef void (*nvnc_key_fn)(struct nvnc_client*, uint32_t key,
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
typedef void (*nvnc_cut_text_fn)(struct nvnc*, const char* text, uint32_t len);
typedef void (*nvnc_fb_release_fn)(struct nvnc_fb*, void* context);
typedef void (*nvnc_cleanup_fn)(void* userdata);

extern const char nvnc_version[];

struct nvnc* nvnc_open(const char* addr, uint16_t port);
struct nvnc* nvnc_open_unix(const char *addr);
void nvnc_close(struct nvnc* self);

void nvnc_add_display(struct nvnc*, struct nvnc_display*);
void nvnc_remove_display(struct nvnc*, struct nvnc_display*);

void nvnc_set_userdata(void* self, void* userdata, nvnc_cleanup_fn);
void* nvnc_get_userdata(const void* self);

struct nvnc* nvnc_client_get_server(const struct nvnc_client* client);

void nvnc_set_name(struct nvnc* self, const char* name);

void nvnc_set_key_fn(struct nvnc* self, nvnc_key_fn);
void nvnc_set_key_code_fn(struct nvnc* self, nvnc_key_fn);
void nvnc_set_pointer_fn(struct nvnc* self, nvnc_pointer_fn);
void nvnc_set_fb_req_fn(struct nvnc* self, nvnc_fb_req_fn);
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn);
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn);
void nvnc_set_cut_text_receive_fn(struct nvnc* self, nvnc_cut_text_fn fn);

bool nvnc_has_auth(void);
int nvnc_enable_auth(struct nvnc* self, const char* privkey_path,
                     const char* cert_path, nvnc_auth_fn, void* userdata);

struct nvnc_fb* nvnc_fb_new(uint16_t width, uint16_t height,
                            uint32_t fourcc_format, uint16_t stride);
struct nvnc_fb* nvnc_fb_from_buffer(void* buffer, uint16_t width,
				    uint16_t height, uint32_t fourcc_format,
				    int32_t stride);
struct nvnc_fb* nvnc_fb_from_gbm_bo(struct gbm_bo* bo);

void nvnc_fb_ref(struct nvnc_fb* fb);
void nvnc_fb_unref(struct nvnc_fb* fb);

void nvnc_fb_set_release_fn(struct nvnc_fb* fb, nvnc_fb_release_fn fn,
			    void* context);
void nvnc_fb_set_transform(struct nvnc_fb* fb, enum nvnc_transform);

void* nvnc_fb_get_addr(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb);
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb);
int32_t nvnc_fb_get_stride(const struct nvnc_fb* fb);
int nvnc_fb_get_pixel_size(const struct nvnc_fb* fb);
struct gbm_bo* nvnc_fb_get_gbm_bo(const struct nvnc_fb* fb);
enum nvnc_transform nvnc_fb_get_transform(const struct nvnc_fb* fb);
enum nvnc_fb_type nvnc_fb_get_type(const struct nvnc_fb* fb);

struct nvnc_fb_pool* nvnc_fb_pool_new(uint16_t width, uint16_t height,
				      uint32_t fourcc_format, uint16_t stride);
bool nvnc_fb_pool_resize(struct nvnc_fb_pool*, uint16_t width, uint16_t height,
			 uint32_t fourcc_format, uint16_t stride);

void nvnc_fb_pool_ref(struct nvnc_fb_pool*);
void nvnc_fb_pool_unref(struct nvnc_fb_pool*);

struct nvnc_fb* nvnc_fb_pool_acquire(struct nvnc_fb_pool*);
void nvnc_fb_pool_release(struct nvnc_fb_pool*, struct nvnc_fb*);

struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos);
void nvnc_display_ref(struct nvnc_display*);
void nvnc_display_unref(struct nvnc_display*);

struct nvnc* nvnc_display_get_server(const struct nvnc_display*);

void nvnc_display_feed_buffer(struct nvnc_display*, struct nvnc_fb*,
			      struct pixman_region16* damage);

void nvnc_send_cut_text(struct nvnc*, const char* text, uint32_t len);
