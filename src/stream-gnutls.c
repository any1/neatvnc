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
#include "stream.h"
#include "stream-common.h"
#include "sys/queue.h"

static int stream__try_tls_accept(struct stream* self);

static int stream_gnutls_close(struct stream* self)
{
	if (self->state == STREAM_STATE_CLOSED)
		return -1;

	self->state = STREAM_STATE_CLOSED;

	while (!TAILQ_EMPTY(&self->send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->send_queue);
		TAILQ_REMOVE(&self->send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_FAILED);
	}

	if (self->tls_session)
		gnutls_deinit(self->tls_session);
	self->tls_session = NULL;

	aml_stop(aml_get_default(), self->handler);
	close(self->fd);
	self->fd = -1;

	return 0;
}

static void stream_gnutls_destroy(struct stream* self)
{
	stream_close(self);
	aml_unref(self->handler);
	free(self);
}

static int stream_gnutls__flush(struct stream* self)
{
	if (self->state != STREAM_STATE_TLS_READY) {
		return 0;
	}

	while (!TAILQ_EMPTY(&self->send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->send_queue);

		ssize_t rc = gnutls_record_send(
			self->tls_session, req->payload->payload,
			req->payload->size);
		if (rc < 0) {
			if (gnutls_error_is_fatal(rc)) {
				stream_close(self);
				return -1;
			}

			stream__poll_rw(self);
			return 0;
		}

		self->bytes_sent += rc;

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

	if (events & AML_EVENT_READ)
		stream_gnutls__on_readable(self);

	if (events & AML_EVENT_WRITE)
		stream_gnutls__on_writable(self);
}

static int stream_gnutls_send(struct stream* self, struct rcbuf* payload,
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

	return stream_gnutls__flush(self);
}

static ssize_t stream_gnutls_read(struct stream* self, void* dst, size_t size)
{
	ssize_t rc = gnutls_record_recv(self->tls_session, dst, size);
	if (rc == 0) {
		stream__remote_closed(self);
		return rc;
	}
	if (rc > 0) {
		self->bytes_received += rc;
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
	assert(gnutls_record_get_direction(self->tls_session) == 0);
	return -1;
}

static int stream__try_tls_accept(struct stream* self)
{
	int rc;

	rc = gnutls_handshake(self->tls_session);
	if (rc == GNUTLS_E_SUCCESS) {
		self->state = STREAM_STATE_TLS_READY;
		stream__poll_r(self);
		stream_gnutls__flush(self);
		return 0;
	}

	if (gnutls_error_is_fatal(rc)) {
		aml_stop(aml_get_default(), self->handler);
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

static struct stream_impl impl = {
	.close = stream_gnutls_close,
	.destroy = stream_gnutls_destroy,
	.read = stream_gnutls_read,
	.send = stream_gnutls_send,
};

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

	aml_stop(aml_get_default(), self->handler);
	aml_unref(self->handler);

	self->handler = aml_handler_new(self->fd, stream_gnutls__on_event, self,
			NULL);
	assert(self->handler);

	rc = aml_start(aml_get_default(), self->handler);
	assert(rc >= 0);

	gnutls_transport_set_int(self->tls_session, self->fd);

	self->impl = &impl;

	return stream__try_tls_accept(self);

failure:
	gnutls_deinit(self->tls_session);
	return -1;
}
