/*
 * Copyright (c) 2020 Andri Yngvason
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

#include <uv.h>

#include "config.h"
#include "sys/queue.h"
#include "rcbuf.h"

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

enum stream_state {
	STREAM_STATE_NORMAL = 0,
	STREAM_STATE_CLOSED,
#ifdef ENABLE_TLS
	STREAM_STATE_TLS_HANDSHAKE,
	STREAM_STATE_TLS_READY,
#endif
};

enum stream_status {
	STREAM_READY = 0,
	STREAM_CLOSED,
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

typedef void (*stream_event_fn)(struct stream*, enum stream_event);
typedef void (*stream_req_fn)(void*, enum stream_req_status);

struct stream_req {
	struct rcbuf* payload;
	stream_req_fn on_done;
	void* userdata;
	TAILQ_ENTRY(stream_req) link;
};

TAILQ_HEAD(stream_send_queue, stream_req);

struct stream {
	enum stream_state state;

	int fd;
	uv_poll_t uv_poll;
	stream_event_fn on_event;
	void* userdata;

	struct stream_send_queue send_queue;

#ifdef ENABLE_TLS
	gnutls_session_t tls_session;
#endif
};

struct stream* stream_new(int fd, stream_event_fn on_event, void* userdata);
int stream_close(struct stream* self);
void stream_destroy(struct stream* self);
ssize_t stream_read(struct stream* self, void* dst, size_t size);
int stream_write(struct stream* self, struct rcbuf* payload,
                 stream_req_fn on_done, void* userdata);

#ifdef ENABLE_TLS
int stream_upgrade_to_tls(struct stream* self, void* context);
#endif
