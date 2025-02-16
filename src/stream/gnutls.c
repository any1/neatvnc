/*
 * Copyright (c) 2020 - 2025 Andri Yngvason
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
#include <limits.h>
#include <aml.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

#include <gnutls/gnutls.h>

#include "rcbuf.h"
#include "stream/stream.h"
#include "stream/common.h"
#include "sys/queue.h"

struct stream_gnutls {
	struct stream base;

	gnutls_session_t session;
};

static_assert(sizeof(struct stream_gnutls) <= STREAM_ALLOC_SIZE,
		"struct stream_gnutls has grown too large, increase STREAM_ALLOC_SIZE");

static int stream__try_tls_accept(struct stream* self);

static int stream_gnutls_close(struct stream* base)
{
	struct stream_gnutls* self = (struct stream_gnutls*)base;

	if (self->base.state == STREAM_STATE_CLOSED)
		return -1;

	self->base.state = STREAM_STATE_CLOSED;

	stream_ref(&self->base);

	while (!TAILQ_EMPTY(&self->base.send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->base.send_queue);
		TAILQ_REMOVE(&self->base.send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_FAILED);
	}

	if (self->session)
		gnutls_deinit(self->session);
	self->session = NULL;

	aml_stop(aml_get_default(), self->base.handler);
	close(self->base.fd);
	self->base.fd = -1;

	// unref
	stream_destroy(&self->base);

	return 0;
}

static void stream_gnutls_destroy(struct stream* self)
{
	stream_close(self);
	aml_unref(self->handler);
	free(self);
}

static int stream_gnutls__flush(struct stream* base)
{
	struct stream_gnutls* self = (struct stream_gnutls*)base;

	stream_ref(base);
	int rc = -1;

	while (!TAILQ_EMPTY(&self->base.send_queue)) {
		assert(self->base.state != STREAM_STATE_CLOSED);

		struct stream_req* req = TAILQ_FIRST(&self->base.send_queue);

		ssize_t n_sent = gnutls_record_send(self->session,
				req->payload->payload, req->payload->size);
		if (n_sent < 0) {
			if (gnutls_error_is_fatal(n_sent)) {
				stream_close(base);
				goto done;
			}

			stream__poll_rw(base);
			rc = 0;
			goto done;
		}

		self->base.bytes_sent += n_sent;

		ssize_t remaining = req->payload->size - n_sent;

		if (remaining > 0) {
			char* p = req->payload->payload;
			size_t s = req->payload->size;
			memmove(p, p + s - remaining, remaining);
			req->payload->size = remaining;
			stream__poll_rw(base);
			rc = 1;
			goto done;
		}

		assert(remaining == 0);

		TAILQ_REMOVE(&self->base.send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_DONE);
	}

	if (TAILQ_EMPTY(&base->send_queue) && base->state != STREAM_STATE_CLOSED)
		stream__poll_r(base);

	rc = 1;
done:
	// unref
	stream_destroy(base);
	return rc;
}

static void stream_gnutls__on_readable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
		/* fallthrough */
	case STREAM_STATE_TLS_READY:
		if (self->on_event)
			self->on_event(self, STREAM_EVENT_READ);
		break;
	case STREAM_STATE_TLS_HANDSHAKE:
		stream__try_tls_accept(self);
		break;
	case STREAM_STATE_CLOSED:
		break;
	}
}

static void stream_gnutls__on_writable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
		/* fallthrough */
	case STREAM_STATE_TLS_READY:
		stream_gnutls__flush(self);
		break;
	case STREAM_STATE_TLS_HANDSHAKE:
		stream__try_tls_accept(self);
		break;
	case STREAM_STATE_CLOSED:
		break;
	}
}

static void stream_gnutls__on_event(void* obj)
{
	struct stream* self = aml_get_userdata(obj);
	uint32_t events = aml_get_revents(obj);

	stream_ref(self);

	if (events & AML_EVENT_READ)
		stream_gnutls__on_readable(self);

	if (events & AML_EVENT_WRITE)
		stream_gnutls__on_writable(self);

	stream_destroy(self);
}

static int stream_gnutls_send(struct stream* self, struct rcbuf* payload,
		stream_req_fn on_done, void* userdata)
{
	if (self->state == STREAM_STATE_CLOSED)
		goto failure;

	struct stream_req* req = calloc(1, sizeof(*req));
	if (!req)
		goto failure;

	req->payload = payload;
	req->on_done = on_done;
	req->userdata = userdata;

	TAILQ_INSERT_TAIL(&self->send_queue, req, link);

	return stream_gnutls__flush(self);

failure:
	rcbuf_unref(payload);
	return -1;
}

static ssize_t stream_gnutls_read(struct stream* base, void* dst, size_t size)
{
	struct stream_gnutls* self = (struct stream_gnutls*)base;

	ssize_t rc = gnutls_record_recv(self->session, dst, size);
	if (rc == 0) {
		stream__remote_closed(base);
		return rc;
	}
	if (rc > 0) {
		self->base.bytes_received += rc;
		return rc;
	}

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
	assert(gnutls_record_get_direction(self->session) == 0);
	return -1;
}

static int stream__try_tls_accept(struct stream* base)
{
	struct stream_gnutls* self = (struct stream_gnutls*)base;
	int rc;

	rc = gnutls_handshake(self->session);
	if (rc == GNUTLS_E_SUCCESS) {
		self->base.state = STREAM_STATE_TLS_READY;
		stream__poll_r(base);
		return 0;
	}

	if (gnutls_error_is_fatal(rc)) {
		aml_stop(aml_get_default(), self->base.handler);
		return -1;
	}

	int was_writing = gnutls_record_get_direction(self->session);
	if (was_writing)
		stream__poll_w(base);
	else
		stream__poll_r(base);

	self->base.state = STREAM_STATE_TLS_HANDSHAKE;
	return 0;
}

static struct stream_impl impl = {
	.close = stream_gnutls_close,
	.destroy = stream_gnutls_destroy,
	.read = stream_gnutls_read,
	.send = stream_gnutls_send,
};

int stream_upgrade_to_tls(struct stream* base, void* context)
{
	struct stream_gnutls* self = (struct stream_gnutls*)base;
	int rc;

	rc = gnutls_init(&self->session, GNUTLS_SERVER | GNUTLS_NONBLOCK);
	if (rc != GNUTLS_E_SUCCESS)
		return -1;

	rc = gnutls_set_default_priority(self->session);
	if (rc != GNUTLS_E_SUCCESS)
		goto failure;

	rc = gnutls_credentials_set(self->session, GNUTLS_CRD_CERTIFICATE,
			context);
	if (rc != GNUTLS_E_SUCCESS)
		goto failure;

	aml_stop(aml_get_default(), self->base.handler);
	aml_unref(self->base.handler);

	self->base.handler = aml_handler_new(self->base.fd,
			stream_gnutls__on_event, self, NULL);
	assert(self->base.handler);

	rc = aml_start(aml_get_default(), self->base.handler);
	assert(rc >= 0);

	gnutls_transport_set_int(self->session, self->base.fd);

	self->base.impl = &impl;

	return stream__try_tls_accept(base);

failure:
	gnutls_deinit(self->session);
	return -1;
}
