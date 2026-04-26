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
		.size = sizeof(ld), \
		.level = lvl, \
		.file = __FILE__, \
		.line = __LINE__, \
	}; \
	nvnc__log(&ld, fmt, ## __VA_ARGS__); \
} while(0)

#ifndef NDEBUG
#define nvnc_trace(fmt, ...) do { \
	struct nvnc_log_data ld = { \
		.size = sizeof(ld), \
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
struct nvnc_frame;
struct nvnc_frame_pool;
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

enum nvnc_buffer_type {
	NVNC_BUFFER_UNSPEC = 0,
	NVNC_BUFFER_SIMPLE,
	NVNC_BUFFER_GBM_BO,
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
	NVNC_LOG_PANIC = 0,   /// Fatal. This will result in SIGABRT
	NVNC_LOG_ERROR = 1,   /// Serious non-fatal problems
	NVNC_LOG_WARNING = 2, /// Sub-optimal conditions
	NVNC_LOG_INFO = 3,    /// Info for regular users
	NVNC_LOG_DEBUG = 4,   /// Info for developers
	NVNC_LOG_TRACE = 5,   /// Spammy logging for developers
};

enum nvnc_auth_flags {
	NVNC_AUTH_REQUIRE_AUTH = 1 << 0,
	NVNC_AUTH_REQUIRE_ENCRYPTION = 1 << 1,
	NVNC_AUTH_ALLOW_BROKEN_CRYPTO = 1 << 2,
	NVNC_AUTH_REQUIRE_USERNAME = 1 << 3,
};

struct nvnc_log_data {
	size_t size;               /// Used by nvnc_default_logger
	enum nvnc_log_level level; /// Log level
	const char* file;          /// Source file from which the message came.
	int line;                  /// Line from which the message came.
};

typedef void (*nvnc_key_fn)(struct nvnc_client*, uint32_t key,
		bool is_pressed);
typedef void (*nvnc_pointer_fn)(struct nvnc_client*, uint16_t x, uint16_t y,
		enum nvnc_button_mask);
typedef void (*nvnc_normalised_pointer_fn)(struct nvnc_client*, double x,
		double y, enum nvnc_button_mask);
typedef void (*nvnc_client_fn)(struct nvnc_client*);
typedef void (*nvnc_damage_fn)(struct pixman_region16* damage, void* userdata);
typedef void (*nvnc_auth_fn)(struct nvnc_auth_creds*, void* userdata);
typedef void (*nvnc_cut_text_fn)(struct nvnc_client*, const char* text,
		uint32_t len);
typedef struct nvnc_buffer* (*nvnc_buffer_alloc_fn)(struct nvnc_buffer_pool*);
typedef void (*nvnc_cleanup_fn)(void* userdata);
typedef void (*nvnc_log_fn)(const struct nvnc_log_data*, const char* message);
typedef bool (*nvnc_desktop_layout_fn)(
		struct nvnc_client*, const struct nvnc_desktop_layout*);

extern const char nvnc_version[];

/**
 * Internal logging function. Use the nvnc_log() macro instead.
 */
void nvnc__log(const struct nvnc_log_data*, const char* fmt, ...);

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
 * Set a callback that is invoked when a new client connects.
 */
void nvnc_set_new_client_fn(struct nvnc* self, nvnc_client_fn);

/**
 * Set a callback that is invoked when the client disconnects.
 *
 * This callback is invoked after the userdata cleanup function set via
 * nvnc_set_userdata.
 */
void nvnc_set_client_cleanup_fn(struct nvnc_client* self, nvnc_client_fn fn);

/**
 * Set a handler for clipboard text received from clients.
 */
void nvnc_set_cut_text_fn(struct nvnc*, nvnc_cut_text_fn fn);

/**
 * Set a handler for desktop layout change requests from clients.
 *
 * Upon receiving a layout change request via this callback, the application
 * must assess whether the requested layout is achievable and/or allowed and
 * return either true or false depending on whether the layout is accepted or
 * not, respectively.
 *
 * The actual layout change need not take place immediately.
 */
void nvnc_set_desktop_layout_fn(struct nvnc* self, nvnc_desktop_layout_fn);

/**
 * Check whether authentication support was compiled in.
 */
bool nvnc_has_auth(void);

/**
 * Enable authentication on the server with the given flags and callback.
 *
 * The flags place constraints on the security methods that are accepted by
 * the server and those constraints are as follows:
 *
 * NVNC_AUTH_REQUIRE_AUTH
 *   The user must authenticate
 *
 * NVNC_AUTH_REQUIRE_ENCRYPTION
 *   Non-encrypted authentication methods are not allowed
 *
 * NVNC_AUTH_ALLOW_BROKEN_CRYPTO
 *   Broken cryptography is allowed. To be used on trusted private networks
 *   only.
 *
 * NVNC_AUTH_REQUIRE_USERNAME
 *   Some security methods only require a password. Those methods will be
 *   excluded if this bit is set.
 *
 * The authentication function is a user-defined callback that must result in
 * either nvnc_auth_creds_accept or nvnc_auth_creds_reject being called, either
 * directly or asynchronously.
 *
 * The credentials object must not be accessed after it has been resolved and
 * it must always be resolved eventually as resources will leak otherwise.
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
 * Accept an authentication request.
 *
 * This resolves the credentials. The object must not be used after this call.
 */
void nvnc_auth_creds_accept(struct nvnc_auth_creds*);

/**
 * Reject an authentication request.
 *
 * This resolves the credentials. The object must not be used after this call.
 */
void nvnc_auth_creds_reject(struct nvnc_auth_creds*, const char* reason);

/**
 * Allocate a new buffer with the given size.
 *
 * The buffer is allocated from main memory using aligned_alloc with an
 * alignment of min(4, sizeof(void*)).
 *
 * The buffer will have a type of NVNC_BUFFER_SIMPLE.
 */
struct nvnc_buffer* nvnc_buffer_new(size_t size);

/**
 * Wrap an external memory address into a buffer object.
 *
 * The buffer will have a type of NVNC_BUFFER_SIMPLE.
 */
struct nvnc_buffer* nvnc_buffer_from_addr(void* address);

/**
 * Wrap a GBM buffer object into a buffer object.
 *
 * The buffer will have a type of NVNC_BUFFER_GBM_BO.
 *
 * The nvnc_buffer object does not take over ownership of the gbm_bo object, so
 * make sure to release/free it inside the cleanup callback for nvnc_buffer.
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
 * Allocate a new frame with the given dimensions and format.
 *
 * A frame is a wrapper around buffer that contains information about how
 * to interpret the buffer, such as, but not limited to: width, height, stride
 * and format.
 *
 * An nvnc_buffer object will be allocated internally by this function with a
 * type of NVNC_BUFFER_SIMPLE. See nvnc_buffer_new();
 */
struct nvnc_frame* nvnc_frame_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride);

/**
 * Create a frame backed by an existing buffer object.
 *
 * This function increases the reference count of the buffer object, so
 * remember to call nvnc_buffer_unref() afterwards.
 */
struct nvnc_frame* nvnc_frame_from_buffer(struct nvnc_buffer* buffer,
		uint16_t width, uint16_t height, uint32_t format,
		int32_t stride);

/**
 * Create a frame from a raw memory address.
 *
 * This function calls nvnc_buffer_from_addr() internally.
 */
struct nvnc_frame* nvnc_frame_from_raw(void* buffer, uint16_t width,
		uint16_t height, uint32_t fourcc_format, int32_t stride);

/**
 * Create a frame from a GBM buffer object.
 *
 * This function calls nvnc_buffer_from_gbm_bo() internally and assigns the
 * dimensions and format of the bo to the frame object.
 */
struct nvnc_frame* nvnc_frame_from_gbm_bo(struct gbm_bo* bo);

/**
 * Increment the reference count of the frame.
 */
void nvnc_frame_ref(struct nvnc_frame* fb);

/**
 * Decrement the reference count of the frame.
 */
void nvnc_frame_unref(struct nvnc_frame* fb);

/**
 * Set the rotation/flip transformation applied to the frame.
 */
void nvnc_frame_set_transform(struct nvnc_frame* fb, enum nvnc_transform);

/**
 * Set the logical width used for display scaling.
 */
void nvnc_frame_set_logical_width(struct nvnc_frame* fb, uint16_t value);

/**
 * Set the logical height used for display scaling.
 */
void nvnc_frame_set_logical_height(struct nvnc_frame* fb, uint16_t value);

/**
 * Set the damage region of the frame. A new frame is assumed to be fully
 * damaged. This narrows it down.
 */
void nvnc_frame_set_damage(struct nvnc_frame*,
		const struct pixman_region16* damage);

/**
 * Set the presentation timestamp of the frame.
 */
void nvnc_frame_set_pts(struct nvnc_frame* fb, uint64_t pts);

/**
 * Get the underlying buffer object of a frame.
 */
struct nvnc_buffer* nvnc_frame_get_buffer(const struct nvnc_frame* fb);

/**
 * Get the memory address of the frame pixel data.
 */
void* nvnc_frame_get_addr(const struct nvnc_frame* fb);

/**
 * Get the width of the frame in pixels.
 */
uint16_t nvnc_frame_get_width(const struct nvnc_frame* fb);

/**
 * Get the height of the frame in pixels.
 */
uint16_t nvnc_frame_get_height(const struct nvnc_frame* fb);

/**
 * Get the logical width of the frame.
 */
uint16_t nvnc_frame_get_logical_width(const struct nvnc_frame* fb);

/**
 * Get the logical height of the frame.
 */
uint16_t nvnc_frame_get_logical_height(const struct nvnc_frame* fb);

/**
 * Get the DRM fourcc pixel format of the frame.
 */
uint32_t nvnc_frame_get_fourcc_format(const struct nvnc_frame* fb);

/**
 * Get the stride (bytes per row) of the frame.
 */
int32_t nvnc_frame_get_stride(const struct nvnc_frame* fb);

/**
 * Get the number of bytes per pixel for the frame's format.
 */
int nvnc_frame_get_pixel_size(const struct nvnc_frame* fb);

/**
 * Get the GBM buffer object backing the frame, if any.
 */
struct gbm_bo* nvnc_frame_get_gbm_bo(const struct nvnc_frame* fb);

/**
 * Get the rotation/flip transformation of the frame.
 */
enum nvnc_transform nvnc_frame_get_transform(const struct nvnc_frame* fb);

/**
 * Get the type of the frame (simple, GBM BO, etc.).
 */
enum nvnc_buffer_type nvnc_frame_get_type(const struct nvnc_frame* fb);

/**
 * Get the presentation timestamp of the frame.
 */
uint64_t nvnc_frame_get_pts(const struct nvnc_frame* fb);

/**
 * Get the damage of the frame.
 */
void nvnc_frame_get_damage(const struct nvnc_frame*,
		struct pixman_region16* damage);

/**
 * Create a frame pool with the given dimensions and format.
 *
 * A frame pool is just a wrapper around a buffer pool that contains the
 * meta data required to allocate an nvnc_buffer and to fill in the same values
 * of an nvnc_frame.
 */
struct nvnc_frame_pool* nvnc_frame_pool_new(uint16_t width, uint16_t height,
		uint32_t fourcc_format, uint16_t stride);

/**
 * Resize the pool's frame parameters. Returns true if changed.
 */
bool nvnc_frame_pool_resize(struct nvnc_frame_pool*, uint16_t width,
		uint16_t height, uint32_t fourcc_format, uint16_t stride);

/**
 * Increment the reference count of the frame pool.
 */
void nvnc_frame_pool_ref(struct nvnc_frame_pool*);

/**
 * Decrement the reference count of the frame pool.
 */
void nvnc_frame_pool_unref(struct nvnc_frame_pool*);

/**
 * Acquire a frame from the pool, allocating a new one if no frame
 * is free.
 */
struct nvnc_frame* nvnc_frame_pool_acquire(struct nvnc_frame_pool*);

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
 * Set the position of the display within the composite layout in logical
 * coordinates
 */
void nvnc_display_set_position(struct nvnc_display* self, uint16_t x,
		uint16_t y);

/**
 * Set the size of the display within the composite layout in logical
 * coordinates.
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
 * Submit a new frame.
 *
 * The submitted frame will become the new current frame for the display and it
 * will remain so until this function is called again or the display is
 * destroyed.
 *
 * If a client is connected, the new frame will be sent to the client as soon as
 * possible unless it is replaced before it can be sent.
 */
void nvnc_display_feed_frame(struct nvnc_display*, struct nvnc_frame*);

/**
 * Get the total desktop width from the layout in logical coordinates.
 */
uint16_t nvnc_desktop_layout_get_width(const struct nvnc_desktop_layout*);

/**
 * Get the total desktop height from the layout in logical coordinates.
 */
uint16_t nvnc_desktop_layout_get_height(const struct nvnc_desktop_layout*);

/**
 * Get the number of displays in the layout.
 */
uint8_t nvnc_desktop_layout_get_display_count(const struct nvnc_desktop_layout*);

/**
 * Get the x position of a display within the layout in logical coordinates.
 */
uint16_t nvnc_desktop_layout_get_display_x_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the y position of a display within the layout in logical coordinates.
 */
uint16_t nvnc_desktop_layout_get_display_y_pos(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the width of a display within the layout in logical coordinates.
 */
uint16_t nvnc_desktop_layout_get_display_width(
		const struct nvnc_desktop_layout*, uint8_t display_index);

/**
 * Get the height of a display within the layout in logical coordinates.
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
 *
 * If is_damaged is not set, the submitted cursor frame will replace the old
 * cursor frame, but it will not be sent to already connected clients. The
 * purpose of this is to allow for proper rotation of buffer pools.
 */
void nvnc_set_cursor(struct nvnc*, struct nvnc_frame*, uint16_t hotspot_x,
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
 * Set a thread-local log handler, overriding the global one for the current
 * thread only.
 */
void nvnc_set_log_fn_thread_local(nvnc_log_fn fn);

/**
 * Set the maximum log level for messages to be emitted.
 */
void nvnc_set_log_level(enum nvnc_log_level);

/**
 * Filter log output to only include messages from matching source files.
 *
 * If the argument is a substring of the source file name, the filter matches.
 */
void nvnc_set_log_filter(const char* value);

/**
 * Rate how well a pixel format is supported for frame encoding.
 *
 * The score is in the closed range between 0.0 and 1.0, from worst to best,
 * respectively.
 *
 * A score of 0.0 means that the pixel format is not supported at all and
 * buffers with that format & modifier pair must not be submitted.
 *
 * The score indicates how well the library is expected to perform with the
 * given pixel format relative to other formats and how well the information
 * inside the buffer will be utilised, also in relation to other formats.
 *
 * For example, if a client is connected that has selected a 16 bpp pixel
 * format and no other client is connected that requires a higher bpp value,
 * RGB565 will receive a higher rating than RGBX8888.
 */
double nvnc_rate_pixel_format(const struct nvnc* self,
		enum nvnc_buffer_type, uint32_t format, uint64_t modifier);

/**
 * Rate how well a pixel format is supported for cursor images.
 *
 * See nvnc_rate_pixel_format.
 *
 * An alpha channel is required for all cursor buffers, so any pixel format
 * without an alpha channel will receive a score of 0.0.
 */
double nvnc_rate_cursor_pixel_format(const struct nvnc* self,
		enum nvnc_buffer_type, uint32_t format, uint64_t modifier);
