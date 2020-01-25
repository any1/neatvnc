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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/uio.h>
#include <uv.h>

#ifdef ENABLE_TLS
#include <gnutls/gnutls.h>
#endif

#include "type-macros.h"
#include "rcbuf.h"
#include "stream.h"
#include "sys/queue.h"

static void stream__on_event(uv_poll_t* uv_poll, int status, int events);
#ifdef ENABLE_TLS
static int stream__try_tls_accept(struct stream* self);
#endif

static inline void stream__poll_r(struct stream* self)
{
	uv_poll_start(&self->uv_poll, UV_READABLE | UV_DISCONNECT,
	              stream__on_event);
}

static inline void stream__poll_w(struct stream* self)
{
	uv_poll_start(&self->uv_poll, UV_WRITABLE | UV_DISCONNECT,
	              stream__on_event);
}

static inline void stream__poll_rw(struct stream* self)
{
	uv_poll_start(&self->uv_poll, UV_READABLE | UV_DISCONNECT | UV_WRITABLE,
	              stream__on_event);
}

static void stream_req__finish(struct stream_req* req, enum stream_req_status status)
{
	if (req->on_done)
		req->on_done(req->userdata, status);

	rcbuf_unref(req->payload);
	free(req);
}

int stream_close(struct stream* self)
{
	if (self->state == STREAM_STATE_CLOSED)
		return -1;

	self->state = STREAM_STATE_CLOSED;

	while (!TAILQ_EMPTY(&self->send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->send_queue);
		TAILQ_REMOVE(&self->send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_FAILED);
	}

#ifdef ENABLE_TLS
	if (self->tls_session)
		gnutls_deinit(self->tls_session);
	self->tls_session = NULL;
#endif

	uv_poll_stop(&self->uv_poll);
	close(self->fd);
	self->fd = -1;

	return 0;
}

void stream__free_poll_handle(uv_handle_t* handle)
{
	uv_poll_t* uv_poll = (uv_poll_t*)handle;
	struct stream* self = container_of(uv_poll, struct stream, uv_poll);
	free(self);
}

void stream_destroy(struct stream* self)
{
	stream_close(self);
	uv_close((uv_handle_t*)&self->uv_poll, stream__free_poll_handle);
}

static void stream__remote_closed(struct stream* self)
{
	stream_close(self);

	if (self->on_event)
		self->on_event(self, STREAM_EVENT_REMOTE_CLOSED);
}

static int stream__flush_plain(struct stream* self)
{
	static struct iovec iov[IOV_MAX];
	size_t n_msgs = 0;
	ssize_t bytes_sent;

	struct stream_req* req;
	TAILQ_FOREACH(req, &self->send_queue, link) {
		iov[n_msgs].iov_base = req->payload->payload;
		iov[n_msgs].iov_len = req->payload->size;

		if (++n_msgs >= IOV_MAX)
			break;
	}

	if (n_msgs < 0)
		return 0;

	bytes_sent = writev(self->fd, iov, n_msgs);
	if (bytes_sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			stream__poll_rw(self);
			errno = EAGAIN;
		} else if (errno == EPIPE) {
			stream__remote_closed(self);
			errno = EPIPE;
		}

		return bytes_sent;
	}

	ssize_t bytes_left = bytes_sent;

	struct stream_req* tmp;
	TAILQ_FOREACH_SAFE(req, &self->send_queue, link, tmp) {
		bytes_left -= req->payload->size;

		if (bytes_left >= 0) {
			TAILQ_REMOVE(&self->send_queue, req, link);
			stream_req__finish(req, STREAM_REQ_DONE);
		} else {
			char* p = req->payload->payload;
			size_t s = req->payload->size;
			memmove(p, p + s + bytes_left, -bytes_left);
			req->payload->size = -bytes_left;
			stream__poll_rw(self);
		}

		if (bytes_left <= 0)
			break;
	}

	if (bytes_left == 0 && self->state != STREAM_STATE_CLOSED)
		stream__poll_r(self);

	assert(bytes_left <= 0);

	return bytes_sent;
}

#ifdef ENABLE_TLS
static int stream__flush_tls(struct stream* self)
{
	while (!TAILQ_EMPTY(&self->send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->send_queue);

		ssize_t rc = gnutls_record_send(
			self->tls_session, req->payload->payload,
			req->payload->size);
		if (rc < 0) {
			gnutls_record_discard_queued(self->tls_session);
			if (gnutls_error_is_fatal(rc))
				stream_close(self);
			return -1;
		}

		ssize_t remaining = req->payload->size - rc;

		if (remaining > 0) {
			char* p = req->payload->payload;
			size_t s = req->payload->size;
			memmove(p, p + s - remaining, remaining);
			req->payload->size = remaining;
			stream__poll_rw(self);
			return 1;
		}

		assert(remaining == 0);

		TAILQ_REMOVE(&self->send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_DONE);
	}

	if (TAILQ_EMPTY(&self->send_queue) && self->state != STREAM_STATE_CLOSED)
		stream__poll_r(self);

	return 1;
}
#endif

static int stream__flush(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL: return stream__flush_plain(self);
#ifdef ENABLE_TLS
	case STREAM_STATE_TLS_READY: return stream__flush_tls(self);
#endif
	default:
		break;
	}
	abort();
	return -1;
}

static void stream__on_readable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
#ifdef ENABLE_TLS
	case STREAM_STATE_TLS_READY:
		if (self->on_event)
			self->on_event(self, STREAM_EVENT_READ);
		break;
	case STREAM_STATE_TLS_HANDSHAKE:
		stream__try_tls_accept(self);
		break;
#endif
	case STREAM_STATE_CLOSED:
		abort();
		break;
	}
}

static void stream__on_writable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
#ifdef ENABLE_TLS
	case STREAM_STATE_TLS_READY:
		stream__flush(self);
		break;
	case STREAM_STATE_TLS_HANDSHAKE:
		stream__try_tls_accept(self);
		break;
#endif
	case STREAM_STATE_CLOSED:
		abort();
		break;
	}
}

static void stream__on_event(uv_poll_t* uv_poll, int status, int events)
{
	struct stream* self = container_of(uv_poll, struct stream, uv_poll);

	if (events & UV_DISCONNECT) {
		stream__remote_closed(self);
		return;
	}

	if (events & UV_READABLE)
		stream__on_readable(self);

	if (events & UV_WRITABLE)
		stream__on_writable(self);
}

struct stream* stream_new(int fd, stream_event_fn on_event, void* userdata)
{
	struct stream* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->fd = fd;
	self->on_event = on_event;
	self->userdata = userdata;

	TAILQ_INIT(&self->send_queue);

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	if (uv_poll_init(uv_default_loop(), &self->uv_poll, fd) < 0)
		goto failure;

	stream__poll_r(self);

	return self;

failure:
	free(self);
	return NULL;
}

int stream_write(struct stream* self, struct rcbuf* payload,
                 stream_req_fn on_done, void* userdata)
{
	if (self->state == STREAM_STATE_CLOSED)
		return -1;

	struct stream_req* req = calloc(1, sizeof(*req));
	if (!req)
		return -1;

	req->payload = payload;
	req->on_done = on_done;
	req->userdata = userdata;

	TAILQ_INSERT_TAIL(&self->send_queue, req, link);

	return stream__flush(self);
}

ssize_t stream__read_plain(struct stream* self, void* dst, size_t size)
{
	return read(self->fd, dst, size);
}

#ifdef ENABLE_TLS
ssize_t stream__read_tls(struct stream* self, void* dst, size_t size)
{
	ssize_t rc = gnutls_record_recv(self->tls_session, dst, size);
	if (rc >= 0)
		return rc;

	switch (rc) {
	case GNUTLS_E_INTERRUPTED:
		errno = EINTR;
		break;
	case GNUTLS_E_AGAIN:
		errno = EAGAIN;
		break;
	default:
		errno = 0;
		break;
	}

	// Make sure data wasn't being written.
	assert(gnutls_record_get_direction(self->tls_session) == 0);
	return -1;
}
#endif

ssize_t stream_read(struct stream* self, void* dst, size_t size)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL: return stream__read_plain(self, dst, size);
#ifdef ENABLE_TLS
	case STREAM_STATE_TLS_READY: return stream__read_tls(self, dst, size);
#endif
	default: break;
	}

	abort();
	return -1;
}

#ifdef ENABLE_TLS
static int stream__try_tls_accept(struct stream* self)
{
	int rc;

	rc = gnutls_handshake(self->tls_session);
	if (rc == GNUTLS_E_SUCCESS) {
		self->state = STREAM_STATE_TLS_READY;
		stream__poll_r(self);
		return 0;
	}

	if (gnutls_error_is_fatal(rc)) {
		uv_poll_stop(&self->uv_poll);
		return -1;
	}

	int was_writing = gnutls_record_get_direction(self->tls_session);
	if (was_writing)
		stream__poll_w(self);
	else
		stream__poll_r(self);

	self->state = STREAM_STATE_TLS_HANDSHAKE;
	return 0;
}

int stream_upgrade_to_tls(struct stream* self, void* context)
{
	int rc;

	rc = gnutls_init(&self->tls_session, GNUTLS_SERVER | GNUTLS_NONBLOCK);
	if (rc != GNUTLS_E_SUCCESS)
		return -1;

	rc = gnutls_set_default_priority(self->tls_session);
	if (rc != GNUTLS_E_SUCCESS)
		goto failure;

	rc = gnutls_credentials_set(self->tls_session, GNUTLS_CRD_CERTIFICATE,
	                            context);
	if (rc != GNUTLS_E_SUCCESS)
		goto failure;

	gnutls_transport_set_int(self->tls_session, self->fd);

	return stream__try_tls_accept(self);

failure:
	gnutls_deinit(self->tls_session);
	return -1;
}
#endif
