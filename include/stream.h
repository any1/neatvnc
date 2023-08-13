/*
 * Copyright (c) 2020 - 2023 Andri Yngvason
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

#include "config.h"
#include "sys/queue.h"
#include "rcbuf.h"
#include "vec.h"

#include <stdint.h>
#include <stdbool.h>

#define STREAM_ALLOC_SIZE 4096

enum stream_state {
	STREAM_STATE_NORMAL = 0,
	STREAM_STATE_CLOSED,
#ifdef ENABLE_TLS
	STREAM_STATE_TLS_HANDSHAKE,
	STREAM_STATE_TLS_READY,
#endif
};

enum stream_req_status {
	STREAM_REQ_DONE = 0,
	STREAM_REQ_FAILED,
};

enum stream_event {
	STREAM_EVENT_READ,
	STREAM_EVENT_REMOTE_CLOSED,
};

struct stream;
struct crypto_cipher;

typedef void (*stream_event_fn)(struct stream*, enum stream_event);
typedef void (*stream_req_fn)(void*, enum stream_req_status);
typedef struct rcbuf* (*stream_exec_fn)(struct stream*, void* userdata);

struct stream_req {
	struct rcbuf* payload;
	stream_req_fn on_done;
	stream_exec_fn exec;
	void* userdata;
	TAILQ_ENTRY(stream_req) link;
};

TAILQ_HEAD(stream_send_queue, stream_req);

struct stream_impl {
	int (*close)(struct stream*);
	void (*destroy)(struct stream*);
	ssize_t (*read)(struct stream*, void* dst, size_t size);
	int (*send)(struct stream*, struct rcbuf* payload,
			stream_req_fn on_done, void* userdata);
	int (*send_first)(struct stream*, struct rcbuf* payload);
	void (*exec_and_send)(struct stream*, stream_exec_fn, void* userdata);
	int (*install_cipher)(struct stream*, struct crypto_cipher*);
};

struct stream {
	struct stream_impl *impl;

	enum stream_state state;

	int fd;
	struct aml_handler* handler;
	stream_event_fn on_event;
	void* userdata;

	struct stream_send_queue send_queue;

	uint32_t bytes_sent;
	uint32_t bytes_received;

	bool cork;

	struct crypto_cipher* cipher;
	struct vec tmp_buf;
};

#ifdef ENABLE_WEBSOCKET
struct stream* stream_ws_new(int fd, stream_event_fn on_event, void* userdata);
#endif

struct stream* stream_new(int fd, stream_event_fn on_event, void* userdata);
int stream_close(struct stream* self);
void stream_destroy(struct stream* self);
ssize_t stream_read(struct stream* self, void* dst, size_t size);
int stream_write(struct stream* self, const void* payload, size_t len,
                 stream_req_fn on_done, void* userdata);
int stream_send(struct stream* self, struct rcbuf* payload,
                stream_req_fn on_done, void* userdata);
int stream_send_first(struct stream* self, struct rcbuf* payload);

// Queue a pure function to be executed when time comes to send it.
void stream_exec_and_send(struct stream* self, stream_exec_fn, void* userdata);

#ifdef ENABLE_TLS
int stream_upgrade_to_tls(struct stream* self, void* context);
#endif

int stream_install_cipher(struct stream* self, struct crypto_cipher* cipher);
