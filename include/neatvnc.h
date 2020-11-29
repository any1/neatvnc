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
struct nvnc_display;
struct nvnc_fb;
struct pixman_region16;

enum nvnc_button_mask {
	NVNC_BUTTON_LEFT = 1 << 0,
	NVNC_BUTTON_MIDDLE = 1 << 1,
	NVNC_BUTTON_RIGHT = 1 << 2,
	NVNC_SCROLL_UP = 1 << 3,
	NVNC_SCROLL_DOWN = 1 << 4,
};

enum nvnc_fb_flags {
	NVNC_FB_PARTIAL = 1 << 0, // The buffer contains only the damaged region
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
typedef void (*nvnc_render_fn)(struct nvnc_display*, struct nvnc_fb*);
typedef void (*nvnc_cut_text_fn)(struct nvnc*, const char* text, uint32_t len);

extern const char nvnc_version[];

struct nvnc* nvnc_open(const char* addr, uint16_t port);
void nvnc_close(struct nvnc* self);

void nvnc_add_display(struct nvnc*, struct nvnc_display*);
void nvnc_remove_display(struct nvnc*, struct nvnc_display*);

void nvnc_set_userdata(void* self, void* userdata);
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
                            uint32_t fourcc_format);

void nvnc_fb_ref(struct nvnc_fb* fb);
void nvnc_fb_unref(struct nvnc_fb* fb);

enum nvnc_fb_flags nvnc_fb_get_flags(const struct nvnc_fb*);
void nvnc_fb_set_flags(struct nvnc_fb*, enum nvnc_fb_flags);

void* nvnc_fb_get_addr(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb);
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb);

struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos);
void nvnc_display_ref(struct nvnc_display*);
void nvnc_display_unref(struct nvnc_display*);

struct nvnc* nvnc_display_get_server(const struct nvnc_display*);

void nvnc_display_set_render_fn(struct nvnc_display* self, nvnc_render_fn fn);
void nvnc_display_set_buffer(struct nvnc_display*, struct nvnc_fb*);

void nvnc_display_damage_region(struct nvnc_display*,
                                const struct pixman_region16*);
void nvnc_display_damage_whole(struct nvnc_display*);

/*
 * Find the regions that differ between fb0 and fb1. Regions outside the hinted
 * rectangle region are not guaranteed to be checked.
 *
 * This is a utility function that may be used to reduce network traffic.
 */
int nvnc_check_damage(struct nvnc_fb* fb0, struct nvnc_fb* fb1,
                      int x_hint, int y_hint, int width_hint, int height_hint,
                      nvnc_damage_fn on_check_done, void* userdata);

void nvnc_send_cut_text(struct nvnc*, const char* text, uint32_t len);
