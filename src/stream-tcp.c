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

#include "rcbuf.h"
#include "stream.h"
#include "stream-common.h"
#include "sys/queue.h"

static int stream_tcp_close(struct stream* self)
{
	if (self->state == STREAM_STATE_CLOSED)
		return -1;

	self->state = STREAM_STATE_CLOSED;

	while (!TAILQ_EMPTY(&self->send_queue)) {
		struct stream_req* req = TAILQ_FIRST(&self->send_queue);
		TAILQ_REMOVE(&self->send_queue, req, link);
		stream_req__finish(req, STREAM_REQ_FAILED);
	}

	aml_stop(aml_get_default(), self->handler);
	close(self->fd);
	self->fd = -1;

	return 0;
}

static void stream_tcp_destroy(struct stream* self)
{
	stream_close(self);
	aml_unref(self->handler);
	free(self);
}

static int stream_tcp__flush(struct stream* self)
{
	if (self->cork)
		return 0;

	static struct iovec iov[IOV_MAX];
	size_t n_msgs = 0;
	ssize_t bytes_sent;

	struct stream_req* req;
	TAILQ_FOREACH(req, &self->send_queue, link) {
		if (req->exec) {
			if (req->payload)
				rcbuf_unref(req->payload);
			req->payload = req->exec(self, req->userdata);
		}

		iov[n_msgs].iov_base = req->payload->payload;
		iov[n_msgs].iov_len = req->payload->size;

		if (++n_msgs >= IOV_MAX)
			break;
	}

	if (n_msgs == 0)
		return 0;

	struct msghdr msghdr = {
		.msg_iov = iov,
		.msg_iovlen = n_msgs,
	};
	bytes_sent = sendmsg(self->fd, &msghdr, MSG_NOSIGNAL);
	if (bytes_sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			stream__poll_rw(self);
			errno = EAGAIN;
			bytes_sent = 0;
		} else if (errno == EPIPE) {
			stream__remote_closed(self);
			errno = EPIPE;
		}

		return bytes_sent;
	}

	self->bytes_sent += bytes_sent;

	ssize_t bytes_left = bytes_sent;

	struct stream_req* tmp;
	TAILQ_FOREACH_SAFE(req, &self->send_queue, link, tmp) {
		bytes_left -= req->payload->size;

		if (bytes_left >= 0) {
			TAILQ_REMOVE(&self->send_queue, req, link);
			stream_req__finish(req, STREAM_REQ_DONE);
		} else {
			if (req->exec) {
				free(req->userdata);
				req->userdata = NULL;
				req->exec = NULL;
			}
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

static void stream_tcp__on_readable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
		/* fallthrough */
		if (self->on_event)
			self->on_event(self, STREAM_EVENT_READ);
		break;
	case STREAM_STATE_CLOSED:
		break;
	default:;
	}
}

static void stream_tcp__on_writable(struct stream* self)
{
	switch (self->state) {
	case STREAM_STATE_NORMAL:
		/* fallthrough */
		stream_tcp__flush(self);
		break;
	case STREAM_STATE_CLOSED:
		break;
	default:;
	}
}

static void stream_tcp__on_event(void* obj)
{
	struct stream* self = aml_get_userdata(obj);
	uint32_t events = aml_get_revents(obj);

	if (events & AML_EVENT_READ)
		stream_tcp__on_readable(self);

	if (events & AML_EVENT_WRITE)
		stream_tcp__on_writable(self);
}

static ssize_t stream_tcp_read(struct stream* self, void* dst, size_t size)
{
	if (self->state != STREAM_STATE_NORMAL)
		return -1;

	ssize_t rc = read(self->fd, dst, size);
	if (rc == 0)
		stream__remote_closed(self);
	if (rc > 0)
		self->bytes_received += rc;
	return rc;
}

static int stream_tcp_send(struct stream* self, struct rcbuf* payload,
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

	return stream_tcp__flush(self);
}

static int stream_tcp_send_first(struct stream* self, struct rcbuf* payload)
{
	if (self->state == STREAM_STATE_CLOSED)
		return -1;

	struct stream_req* req = calloc(1, sizeof(*req));
	if (!req)
		return -1;

	req->payload = payload;
	TAILQ_INSERT_HEAD(&self->send_queue, req, link);

	return stream_tcp__flush(self);
}

static void stream_tcp_exec_and_send(struct stream* self,
		stream_exec_fn exec_fn, void* userdata)
{
	if (self->state == STREAM_STATE_CLOSED)
		return;

	struct stream_req* req = calloc(1, sizeof(*req));
	if (!req)
		return;

	req->exec = exec_fn;
	req->userdata = userdata;

	TAILQ_INSERT_TAIL(&self->send_queue, req, link);

	stream_tcp__flush(self);
}

static struct stream_impl impl = {
	.close = stream_tcp_close,
	.destroy = stream_tcp_destroy,
	.read = stream_tcp_read,
	.send = stream_tcp_send,
	.send_first = stream_tcp_send_first,
	.exec_and_send = stream_tcp_exec_and_send,
};

struct stream* stream_new(int fd, stream_event_fn on_event, void* userdata)
{
	struct stream* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->impl = &impl,
	self->fd = fd;
	self->on_event = on_event;
	self->userdata = userdata;

	TAILQ_INIT(&self->send_queue);

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	self->handler = aml_handler_new(fd, stream_tcp__on_event, self, NULL);
	if (!self->handler)
		goto failure;

	if (aml_start(aml_get_default(), self->handler) < 0)
		goto start_failure;

	stream__poll_r(self);

	return self;

start_failure:
	aml_unref(self->handler);
	self = NULL; /* Handled in unref */
failure:
	free(self);
	return NULL;
}
