/*
 * Copyright (c) 2019 - 2026 Andri Yngvason
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
#include <sys/socket.h>

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

#define nvnc_assert(statement, fmt, ...) \
	if (!(statement)) \
		nvnc_log(NVNC_LOG_PANIC, fmt, ## __VA_ARGS__)

struct nvnc;
struct nvnc_client;
struct nvnc_auth_creds;
struct nvnc_desktop_layout;
struct nvnc_display;
struct nvnc_fb;
struct nvnc_fb_pool;
struct nvnc_buffer;
struct nvnc_buffer_pool;
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
	NVNC_BUTTON_BACK = 1 << 7,
	NVNC_BUTTON_FORWARD = 1 << 8,
};

enum nvnc_fb_type {
	NVNC_FB_UNSPEC = 0,
	NVNC_FB_SIMPLE,
	NVNC_FB_GBM_BO,
};

enum nvnc_stream_type {
	NVNC_STREAM_NORMAL = 0,
	NVNC_STREAM_WEBSOCKET,
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

enum nvnc_keyboard_led_state {
	NVNC_KEYBOARD_LED_SCROLL_LOCK = 1 << 0,
	NVNC_KEYBOARD_LED_NUM_LOCK = 1 << 1,
	NVNC_KEYBOARD_LED_CAPS_LOCK = 1 << 2,
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
	NVNC_AUTH_ALLOW_BROKEN_CRYPTO = 1 << 2,
	NVNC_AUTH_REQUIRE_USERNAME = 1 << 3,
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
typedef void (*nvnc_normalised_pointer_fn)(struct nvnc_client*, double x,
		double y, enum nvnc_button_mask);
typedef void (*nvnc_fb_req_fn)(struct nvnc_client*, bool is_incremental,
                               uint16_t x, uint16_t y, uint16_t width,
                               uint16_t height);
typedef void (*nvnc_client_fn)(struct nvnc_client*);
typedef void (*nvnc_damage_fn)(struct pixman_region16* damage, void* userdata);
typedef bool (*nvnc_auth_fn)(const struct nvnc_auth_creds*, void* userdata);
typedef void (*nvnc_cut_text_fn)(struct nvnc_client*, const char* text,
		uint32_t len);
typedef struct nvnc_buffer* (*nvnc_buffer_alloc_fn)(struct nvnc_buffer_pool*);
typedef void (*nvnc_cleanup_fn)(void* userdata);
typedef void (*nvnc_log_fn)(const struct nvnc_log_data*, const char* message);
typedef bool (*nvnc_desktop_layout_fn)(
		struct nvnc_client*, const struct nvnc_desktop_layout*);

extern const char nvnc_version[];

/**
 * Create a new VNC server instance.
 */
struct nvnc* nvnc_new(void);

/**
 * Destroy the server and close all client connections.
 */
void nvnc_del(struct nvnc* self);

/**
 * Start accepting connections on an existing file descriptor.
 */
int nvnc_listen(struct nvnc* self, int fd, enum nvnc_stream_type type);

/**
 * Create a TCP socket on the given address and port and start listening.
 */
int nvnc_listen_tcp(struct nvnc* self, const char* addr, uint16_t port,
		enum nvnc_stream_type type);

/**
 * Create a Unix domain socket at the given path and start listening.
 */
int nvnc_listen_unix(struct nvnc* self, const char* path,
		enum nvnc_stream_type type);

/**
 * Register a display with the server.
 */
void nvnc_add_display(struct nvnc*, struct nvnc_display*);

/**
 * Unregister a display from the server.
 */
void nvnc_remove_display(struct nvnc*, struct nvnc_display*);

/**
 * Attach custom userdata to any neatvnc object with an optional cleanup
 * callback.
 */
void nvnc_set_userdata(void* self, void* userdata, nvnc_cleanup_fn);

/**
 * Retrieve the custom userdata attached to a neatvnc object.
 */
void* nvnc_get_userdata(const void* self);

/**
 * Get the server instance to which the client belongs.
 */
struct nvnc* nvnc_client_get_server(const struct nvnc_client* client);

/**
 * Check whether the client supports cursor encoding.
 */
bool nvnc_client_supports_cursor(const struct nvnc_client* client);

/**
 * Get the network address of the client.
 */
int nvnc_client_get_address(const struct nvnc_client* client,
		struct sockaddr* restrict addr, socklen_t* restrict addrlen);

/**
 * Get the username used by the client during authentication.
 */
const char* nvnc_client_get_auth_username(const struct nvnc_client* client);

/**
 * Get the first client connected to the server.
 */
struct nvnc_client* nvnc_client_first(struct nvnc* self);

/**
 * Get the next client in the server's client list.
 */
struct nvnc_client* nvnc_client_next(struct nvnc_client* client);

/**
 * Close the client connection.
 */
void nvnc_client_close(struct nvnc_client* client);

/**
 * Set keyboard LED state (caps/num/scroll lock) to send to the client.
 */
void nvnc_client_set_led_state(struct nvnc_client*,
		enum nvnc_keyboard_led_state);

/**
 * Set the desktop name advertised to VNC clients.
 *
 * This name is usually displayed in the title bar of the client's window.
 */
void nvnc_set_name(struct nvnc* self, const char* name);

/**
 * Set a handler for keyboard keysym events.
 */
void nvnc_set_key_fn(struct nvnc* self, nvnc_key_fn);

/**
 * Set a handler for keyboard keycode events.
 */
void nvnc_set_key_code_fn(struct nvnc* self, nvnc_key_fn);

/**
 * Set a handler for pointer movement and button events.
 *
 * The callback will receive the pointer position in absolute logical
 * coordinates.
 */
void nvnc_set_pointer_fn(struct nvnc* self, nvnc_pointer_fn);

/**
 * Set a handler for normalised pointer events.
 *
 * The callback will receive the pointer posision in absolute normalised
 * coordiantes in the closed range from 0.0 to 1.0.
 */
void nvnc_set_normalised_pointer_fn(struct nvnc* self,
		nvnc_normalised_pointer_fn);

/**
 * Set a handler for framebuffer update requests.
 */
void nvnc_set_fb_req_fn(struct nvnc* self, nvnc_fb_req_fn);

/**
 * Set a handler that is invoked when a new client connects.
 */
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn);

/**
 * Set a per-client cleanup handler invoked when the client disconnects.
 */
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn);

/**
 * Set a handler for clipboard text received from clients.
 */
void nvnc_set_cut_text_fn(struct nvnc*, nvnc_cut_text_fn fn);

/**
 * Set a handler for desktop layout change requests from clients.
 */
void nvnc_set_desktop_layout_fn(struct nvnc* self, nvnc_desktop_layout_fn);

/**
 * Check whether authentication support was compiled in.
 */
bool nvnc_has_auth(void);

/**
 * Enable authentication on the server with the given flags and callback.
 */
int nvnc_enable_auth(struct nvnc* self, enum nvnc_auth_flags flags,
		nvnc_auth_fn, void* userdata);

/**
 * Load TLS certificate and private key for encrypted connections.
 */
int nvnc_set_tls_creds(struct nvnc* self, const char* privkey_path,
		const char* cert_path);

/**
 * Load an RSA private key for RSA-AES authentication.
 */
int nvnc_set_rsa_creds(struct nvnc* self, const char* private_key_path);

/**
 * Verify a password against authentication credentials.
 */
bool nvnc_auth_creds_verify(const struct nvnc_auth_creds*,
		const char* password);

/**
 * Get the username from the authentication credentials.
 */
const char* nvnc_auth_creds_get_username(const struct nvnc_auth_creds*);

/**
 * Get the plaintext password from the authentication credentials.
 */
const char* nvnc_auth_creds_get_password(const struct nvnc_auth_creds*);

/**
 * Allocate a new buffer with the given size.
 *
 * The buffer is allocated from main memory using aligned_alloc with an
 * alignment of min(4, sizeof(void*)).
 *
 * The buffer will have a type of NVNC_FB_SIMPLE.
 */
struct nvnc_buffer* nvnc_buffer_new(size_t size);

/**
 * Wrap an external memory address into a buffer object.
 *
 * The buffer will have a type of NVNC_FB_SIMPLE.
 */
struct nvnc_buffer* nvnc_buffer_from_addr(void* address);

/**
 * Wrap a GBM buffer object into a buffer object.
 *
 * The buffer will have a type of NVNC_FB_GBM_BO.
 */
struct nvnc_buffer* nvnc_buffer_from_gbm_bo(struct gbm_bo* bo);

/**
 * Increment the reference count of the buffer.
 */
void nvnc_buffer_ref(struct nvnc_buffer* self);

/**
 * Decrement the reference count of the buffer.
 *
 * Once the reference count reaches 0, the buffer will be destroyed unless it
 * came from a buffer pool. In that case, the count will be reset to 1 and the
 * buffer will be re-inserted into the buffer pool.
 *
 * If the buffer has been assigned a cleanup function, the buffer can also be
 * saved from its fate from within the cleanup function by calling
 * nvnc_buffer_ref on the buffer. This is useful for creating custom buffer
 * pools.
 */
void nvnc_buffer_unref(struct nvnc_buffer* self);

/**
 * Create a buffer pool with a custom allocation callback.
 */
struct nvnc_buffer_pool* nvnc_buffer_pool_new(nvnc_buffer_alloc_fn);

/**
 * Increment the reference count of the buffer pool.
 */
void nvnc_buffer_pool_ref(struct nvnc_buffer_pool*);

/**
 * Decrement the reference count of the buffer pool.
 *
 * The buffer pool need not stay alive while buffer originating from the pool
 * are in-flight. If the buffer pool is destroyed while buffers are in-flight,
 * those buffers will be destroyed instead of being re-inserted into the pool.
 */
void nvnc_buffer_pool_unref(struct nvnc_buffer_pool*);

/**
 * Acquire a buffer from the pool, allocating a new one if needed.
 */
struct nvnc_buffer* nvnc_buffer_pool_acquire(struct nvnc_buffer_pool*);

/**
 * Allocate a new framebuffer with the given dimensions and format.
 *
 * A framebuffer is a wrapper around buffer that contains information about how
 * to interpret the buffer, such as, but not limited to: width, height, stride
 * and format.
 *
 * An nvnc_buffer object will be allocated internally by this function with a
 * type of NVNC_FB_SIMPLE. See nvnc_buffer_new();
 */
struct nvnc_fb* nvnc_fb_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride);

/**
 * Create a framebuffer backed by an existing buffer object.
 *
 * This function increases the reference count of the buffer object, so
 * remember to call nvnc_buffer_unref() afterwards.
 */
struct nvnc_fb* nvnc_fb_from_buffer(struct nvnc_buffer* buffer, uint16_t width,
		uint16_t height, uint32_t format, int16_t stride);

/**
 * Create a framebuffer from a raw memory address.
 *
 * This function calls nvnc_buffer_from_addr() internally.
 */
struct nvnc_fb* nvnc_fb_from_raw(void* buffer, uint16_t width, uint16_t height,
		uint32_t fourcc_format, int32_t stride);

/**
 * Create a framebuffer from a GBM buffer object.
 *
 * This function calls nvnc_buffer_from_gbm_bo() internally and assigns the
 * dimensions and format of the bo to the framebuffer object.
 */
struct nvnc_fb* nvnc_fb_from_gbm_bo(struct gbm_bo* bo);

/**
 * Increment the reference count of the framebuffer.
 */
void nvnc_fb_ref(struct nvnc_fb* fb);

/**
 * Decrement the reference count of the framebuffer.
 */
void nvnc_fb_unref(struct nvnc_fb* fb);

/**
 * Set the rotation/flip transformation applied to the framebuffer.
 */
void nvnc_fb_set_transform(struct nvnc_fb* fb, enum nvnc_transform);

/**
 * Set the logical width used for display scaling.
 */
void nvnc_fb_set_logical_width(struct nvnc_fb* fb, uint16_t value);

/**
 * Set the logical height used for display scaling.
 */
void nvnc_fb_set_logical_height(struct nvnc_fb* fb, uint16_t value);

/**
 * Set the presentation timestamp of the framebuffer.
 */
void nvnc_fb_set_pts(struct nvnc_fb* fb, uint64_t pts);

/**
 * Get the underlying buffer object of a framebuffer.
 */
struct nvnc_buffer* nvnc_fb_get_buffer(const struct nvnc_fb* fb);

/**
 * Get the memory address of the framebuffer pixel data.
 */
void* nvnc_fb_get_addr(const struct nvnc_fb* fb);

/**
 * Get the width of the framebuffer in pixels.
 */
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb);

/**
 * Get the height of the framebuffer in pixels.
 */
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb);

/**
 * Get the logical width of the framebuffer.
 */
uint16_t nvnc_fb_get_logical_width(const struct nvnc_fb* fb);

/**
 * Get the logical height of the framebuffer.
 */
uint16_t nvnc_fb_get_logical_height(const struct nvnc_fb* fb);

/**
 * Get the DRM fourcc pixel format of the framebuffer.
 */
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb);

/**
 * Get the stride (bytes per row) of the framebuffer.
 */
int32_t nvnc_fb_get_stride(const struct nvnc_fb* fb);

/**
 * Get the number of bytes per pixel for the framebuffer's format.
 */
int nvnc_fb_get_pixel_size(const struct nvnc_fb* fb);

/**
 * Get the GBM buffer object backing the framebuffer, if any.
 */
struct gbm_bo* nvnc_fb_get_gbm_bo(const struct nvnc_fb* fb);

/**
 * Get the rotation/flip transformation of the framebuffer.
 */
enum nvnc_transform nvnc_fb_get_transform(const struct nvnc_fb* fb);

/**
 * Get the type of the framebuffer (simple, GBM BO, etc.).
 */
enum nvnc_fb_type nvnc_fb_get_type(const struct nvnc_fb* fb);

/**
 * Get the presentation timestamp of the framebuffer.
 */
uint64_t nvnc_fb_get_pts(const struct nvnc_fb* fb);

/**
 * Create a framebuffer pool with the given dimensions and format.
 *
 * A framebuffer pool is just a wrapper around a buffer pool that contains the
 * meta data required to allocate an nvnc_buffer and to fill in the same values
 * of an nvnc_fb.
 */
struct nvnc_fb_pool* nvnc_fb_pool_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride);

/**
 * Resize the pool's framebuffer parameters. Returns true if changed.
 */
bool nvnc_fb_pool_resize(struct nvnc_fb_pool*, uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride);

/**
 * Increment the reference count of the framebuffer pool.
 */
void nvnc_fb_pool_ref(struct nvnc_fb_pool*);

/**
 * Decrement the reference count of the framebuffer pool.
 */
void nvnc_fb_pool_unref(struct nvnc_fb_pool*);

/**
 * Acquire a framebuffer from the pool, allocating a new one if no framebuffer
 * is free.
 */
struct nvnc_fb* nvnc_fb_pool_acquire(struct nvnc_fb_pool*);

/**
 * Create a display at the given position in the composite layout.
 *
 * A display usually corresponds to a physical monitor.
 */
struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos);

/**
 * Increment the reference count of the display.
 */
void nvnc_display_ref(struct nvnc_display*);

/**
 * Decrement the reference count of the display.
 */
void nvnc_display_unref(struct nvnc_display*);

/**
 * Set the position of the display in the composite layout.
 */
void nvnc_display_set_position(struct nvnc_display* self, uint16_t x,
		uint16_t y);

/**
 * Set the size of the display within the composite layout.
 *
 * If the size does not match the size of the buffer fed to the display, the
 * buffer will be scaled to match the logical size.
 */
void nvnc_display_set_logical_size(struct nvnc_display* self, uint16_t width,
		uint16_t height);

/**
 * Get the server with which the display is registered.
 */
struct nvnc* nvnc_display_get_server(const struct nvnc_display*);

/**
 * Submit a framebuffer with a damage region for encoding and transmission
 * to clients.
 */
void nvnc_display_feed_buffer(struct nvnc_display*, struct nvnc_fb*,
		struct pixman_region16* damage);

/**
 * Get the total desktop width from the layout.
 */
uint16_t nvnc_desktop_layout_get_width(const struct nvnc_desktop_layout*);

/**
 * Get the total desktop height from the layout.
 */
uint16_t nvnc_desktop_layout_get_height(const struct nvnc_desktop_layout*);

/**
 * Get the number of displays in the layout.
 */
uint8_t nvnc_desktop_layout_get_display_count(const struct nvnc_desktop_layout*);

/**
 * Get the x position of a display within the layout.
 */
uint16_t nvnc_desktop_layout_get_display_x_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the y position of a display within the layout.
 */
uint16_t nvnc_desktop_layout_get_display_y_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the width of a display within the layout.
 */
uint16_t nvnc_desktop_layout_get_display_width(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the height of a display within the layout.
 */
uint16_t nvnc_desktop_layout_get_display_height(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the display object at the given index within the layout.
 */
struct nvnc_display* nvnc_desktop_layout_get_display(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Broadcast clipboard text to all connected clients.
 */
void nvnc_send_cut_text(struct nvnc*, const char* text, uint32_t len);

/**
 * Set the cursor image and hotspot; set is_damaged to trigger an update.
 */
void nvnc_set_cursor(struct nvnc*, struct nvnc_fb*, uint16_t hotspot_x,
		uint16_t hotspot_y, bool is_damaged);

/**
 * Default log handler that prints to stderr with file and line info.
 */
void nvnc_default_logger(const struct nvnc_log_data* meta, const char* message);

/**
 * Set the global log handler function.
 */
void nvnc_set_log_fn(nvnc_log_fn);

/**
 * Set a thread-local log handler, overriding the global one.
 */
void nvnc_set_log_fn_thread_local(nvnc_log_fn fn);

/**
 * Set the minimum log level for messages to be emitted.
 */
void nvnc_set_log_level(enum nvnc_log_level);

/**
 * Filter log output to only include messages from matching source files.
 *
 * If the argument is a substring of the source file name, the filter matches.
 */
void nvnc_set_log_filter(const char* value);

/**
 * Internal logging function. Use the nvnc_log() macro instead.
 */
void nvnc__log(const struct nvnc_log_data*, const char* fmt, ...);

/**
 * Rate how well a pixel format is supported for framebuffer encoding.
 *
 * The score is in the closed range between 0.0 and 1.0.
 */
double nvnc_rate_pixel_format(const struct nvnc* self,
		enum nvnc_fb_type fb_type, uint32_t format, uint64_t modifier);

/**
 * Rate how well a pixel format is supported for cursor images.
 *
 * The score is in the closed range between 0.0 and 1.0.
 */
double nvnc_rate_cursor_pixel_format(const struct nvnc* self,
		enum nvnc_fb_type fb_type, uint32_t format, uint64_t modifier);
