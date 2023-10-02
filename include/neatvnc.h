/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>

#define NVNC_NO_PTS UINT64_MAX

#define nvnc_log(lvl, fmt, ...) do { \
	assert(lvl != NVNC_LOG_TRACE); \
	struct nvnc_log_data ld = { \
		.level = lvl, \
		.file = __FILE__, \
		.line = __LINE__, \
	}; \
	nvnc__log(&ld, fmt, ## __VA_ARGS__); \
} while(0)

#ifndef NDEBUG
#define nvnc_trace(fmt, ...) do { \
	struct nvnc_log_data ld = { \
		.level = NVNC_LOG_TRACE, \
		.file = __FILE__, \
		.line = __LINE__, \
	}; \
	nvnc__log(&ld, fmt, ## __VA_ARGS__); \
} while(0)
#else
#define nvnc_trace(...)
#endif

struct nvnc;
struct nvnc_client;
struct nvnc_desktop_layout;
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
	NVNC_SCROLL_LEFT = 1 << 5,
	NVNC_SCROLL_RIGHT = 1 << 6,
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

enum nvnc_log_level {
	NVNC_LOG_PANIC = 0,
	NVNC_LOG_ERROR = 1,
	NVNC_LOG_WARNING = 2,
	NVNC_LOG_INFO = 3,
	NVNC_LOG_DEBUG = 4,
	NVNC_LOG_TRACE = 5,
};

enum nvnc_auth_flags {
	NVNC_AUTH_REQUIRE_AUTH = 1 << 0,
	NVNC_AUTH_REQUIRE_ENCRYPTION = 1 << 1,
};

struct nvnc_log_data {
	enum nvnc_log_level level;
	const char* file;
	int line;
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
typedef void (*nvnc_cut_text_fn)(struct nvnc_client*, const char* text,
		uint32_t len);
typedef void (*nvnc_fb_release_fn)(struct nvnc_fb*, void* context);
typedef struct nvnc_fb* (*nvnc_fb_alloc_fn)(uint16_t width, uint16_t height,
		uint32_t format, uint16_t stride);
typedef void (*nvnc_cleanup_fn)(void* userdata);
typedef void (*nvnc_log_fn)(const struct nvnc_log_data*, const char* message);
typedef bool (*nvnc_desktop_layout_fn)(
		struct nvnc_client*, const struct nvnc_desktop_layout*);

extern const char nvnc_version[];

struct nvnc* nvnc_open(const char* addr, uint16_t port);
struct nvnc* nvnc_open_unix(const char *addr);
struct nvnc* nvnc_open_websocket(const char* addr, uint16_t port);
void nvnc_close(struct nvnc* self);

void nvnc_add_display(struct nvnc*, struct nvnc_display*);
void nvnc_remove_display(struct nvnc*, struct nvnc_display*);

void nvnc_set_userdata(void* self, void* userdata, nvnc_cleanup_fn);
void* nvnc_get_userdata(const void* self);

struct nvnc* nvnc_client_get_server(const struct nvnc_client* client);
bool nvnc_client_supports_cursor(const struct nvnc_client* client);
const char* nvnc_client_get_hostname(const struct nvnc_client* client);
const char* nvnc_client_get_auth_username(const struct nvnc_client* client);

struct nvnc_client* nvnc_client_first(struct nvnc* self);
struct nvnc_client* nvnc_client_next(struct nvnc_client* client);
void nvnc_client_close(struct nvnc_client* client);

void nvnc_set_name(struct nvnc* self, const char* name);

void nvnc_set_key_fn(struct nvnc* self, nvnc_key_fn);
void nvnc_set_key_code_fn(struct nvnc* self, nvnc_key_fn);
void nvnc_set_pointer_fn(struct nvnc* self, nvnc_pointer_fn);
void nvnc_set_fb_req_fn(struct nvnc* self, nvnc_fb_req_fn);
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn);
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn);
void nvnc_set_cut_text_fn(struct nvnc*, nvnc_cut_text_fn fn);
void nvnc_set_desktop_layout_fn(struct nvnc* self, nvnc_desktop_layout_fn);

bool nvnc_has_auth(void);
int nvnc_enable_auth(struct nvnc* self, enum nvnc_auth_flags flags,
		nvnc_auth_fn, void* userdata);
int nvnc_set_tls_creds(struct nvnc* self, const char* privkey_path,
                     const char* cert_path);
int nvnc_set_rsa_creds(struct nvnc* self, const char* private_key_path);

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

void nvnc_fb_set_pts(struct nvnc_fb* fb, uint64_t pts);

void* nvnc_fb_get_addr(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb);
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb);
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb);
int32_t nvnc_fb_get_stride(const struct nvnc_fb* fb);
int nvnc_fb_get_pixel_size(const struct nvnc_fb* fb);
struct gbm_bo* nvnc_fb_get_gbm_bo(const struct nvnc_fb* fb);
enum nvnc_transform nvnc_fb_get_transform(const struct nvnc_fb* fb);
enum nvnc_fb_type nvnc_fb_get_type(const struct nvnc_fb* fb);
uint64_t nvnc_fb_get_pts(const struct nvnc_fb* fb);

struct nvnc_fb_pool* nvnc_fb_pool_new(uint16_t width, uint16_t height,
				      uint32_t fourcc_format, uint16_t stride);
bool nvnc_fb_pool_resize(struct nvnc_fb_pool*, uint16_t width, uint16_t height,
			 uint32_t fourcc_format, uint16_t stride);

void nvnc_fb_pool_set_alloc_fn(struct nvnc_fb_pool*, nvnc_fb_alloc_fn);

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

uint16_t nvnc_desktop_layout_get_width(const struct nvnc_desktop_layout*);
uint16_t nvnc_desktop_layout_get_height(const struct nvnc_desktop_layout*);
uint8_t nvnc_desktop_layout_get_display_count(const struct nvnc_desktop_layout*);
uint16_t nvnc_desktop_layout_get_display_x_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);
uint16_t nvnc_desktop_layout_get_display_y_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);
uint16_t nvnc_desktop_layout_get_display_width(
		const struct nvnc_desktop_layout*, uint8_t display_index);
uint16_t nvnc_desktop_layout_get_display_height(
		const struct nvnc_desktop_layout*, uint8_t display_index);
struct nvnc_display* nvnc_desktop_layout_get_display(
		const struct nvnc_desktop_layout*, uint8_t display_index);

void nvnc_send_cut_text(struct nvnc*, const char* text, uint32_t len);

void nvnc_set_cursor(struct nvnc*, struct nvnc_fb*, uint16_t width,
		     uint16_t height, uint16_t hotspot_x, uint16_t hotspot_y,
		     bool is_damaged);

void nvnc_set_log_fn(nvnc_log_fn);
void nvnc_set_log_level(enum nvnc_log_level);
void nvnc__log(const struct nvnc_log_data*, const char* fmt, ...);
