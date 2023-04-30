/*
 * Copyright (c) 2023 Andri Yngvason
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

#include "stream.h"
#include "stream-common.h"
#include "websocket.h"
#include "vec.h"
#include "neatvnc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>

enum stream_ws_state {
	STREAM_WS_STATE_HANDSHAKE = 0,
	STREAM_WS_STATE_READY,
};

struct stream_ws_exec_ctx {
	stream_exec_fn exec;
	void* userdata;
};

struct stream_ws {
	struct stream base;
	enum stream_ws_state ws_state;
	struct ws_frame_header header;
	enum ws_opcode current_opcode;
	uint8_t read_buffer[4096]; // TODO: Is this a reasonable size?
	size_t read_index;
	struct stream* tcp_stream;
};

static int stream_ws_close(struct stream* self)
{
	struct stream_ws* ws = (struct stream_ws*)self;
	self->state = STREAM_STATE_CLOSED;
	return stream_close(ws->tcp_stream);
}

static void stream_ws_destroy(struct stream* self)
{
	struct stream_ws* ws = (struct stream_ws*)self;
	stream_destroy(ws->tcp_stream);
	free(self);
}

static void stream_ws_read_into_buffer(struct stream_ws* ws)
{
	ssize_t n_read = stream_read(ws->tcp_stream,
			ws->read_buffer + ws->read_index,
			sizeof(ws->read_buffer) - ws->read_index);
	if (n_read > 0)
		ws->read_index += n_read;
}

static void stream_ws_advance_read_buffer(struct stream_ws* ws, size_t size,
		size_t offset)
{
	size_t payload_len = MIN(size, ws->read_index - offset);
	payload_len = MIN(payload_len, ws->header.payload_length);

	ws->read_index -= offset + payload_len;
	memmove(ws->read_buffer, ws->read_buffer + offset + payload_len,
			ws->read_index);
	ws->header.payload_length -= payload_len;
}

static ssize_t stream_ws_copy_payload(struct stream_ws* ws, void* dst,
		size_t size, size_t offset)
{
	size_t payload_len = MIN(size, ws->read_index - offset);
	payload_len = MIN(payload_len, ws->header.payload_length);

	ws_copy_payload(&ws->header, dst, ws->read_buffer + offset, payload_len);
	stream_ws_advance_read_buffer(ws, size, offset);
	return payload_len;
}

static ssize_t stream_ws_process_ping(struct stream_ws* ws, size_t offset)
{
	if (offset > 0) {
		// This means we're at the start, so send a header
		struct ws_frame_header reply = {
			.fin = true,
			.opcode = WS_OPCODE_PONG,
			.payload_length = ws->header.payload_length,
		};

		uint8_t buf[WS_HEADER_MIN_SIZE];
		int reply_len = ws_write_frame_header(buf, &reply);
		stream_write(ws->tcp_stream, buf, reply_len, NULL, NULL);
	}

	int payload_len = MIN(ws->read_index, ws->header.payload_length);

	// Feed back the payload:
	stream_write(ws->tcp_stream, ws->read_buffer + offset,
			payload_len, NULL, NULL);

	stream_ws_advance_read_buffer(ws, payload_len, offset);
	return 0;
}

static ssize_t stream_ws_process_payload(struct stream_ws* ws, void* dst,
		size_t size, size_t offset)
{
	switch (ws->current_opcode) {
	case WS_OPCODE_CONT:
		// Remote end started with a continuation frame. This is
		// unexpected, so we'll just close.
		stream__remote_closed(ws->tcp_stream);
		return 0;
	case WS_OPCODE_TEXT:
		// This is unexpected, but let's just ignore it...
		stream_ws_advance_read_buffer(ws, SIZE_MAX, offset);
		return 0;
	case WS_OPCODE_BIN:
		return stream_ws_copy_payload(ws, dst, size, offset);
	case WS_OPCODE_CLOSE:
		stream__remote_closed(ws->tcp_stream);
		return 0;
	case WS_OPCODE_PING:
		return stream_ws_process_ping(ws, offset);
	case WS_OPCODE_PONG:
		// Don't care
		stream_ws_advance_read_buffer(ws, SIZE_MAX, offset);
		return 0;
	}
	return -1;
}

/* We don't really care about framing. The binary data is just passed on as it
 * arrives and it's not gathered into individual frames.
 */
static ssize_t stream_ws_read_frame(struct stream_ws* ws, void* dst,
		size_t size)
{
	if (ws->header.payload_length > 0) {
		nvnc_trace("Processing left-over payload chunk");
		return stream_ws_process_payload(ws, dst, size, 0);
	}

	if (!ws_parse_frame_header(&ws->header, ws->read_buffer,
				ws->read_index)) {
		return 0;
	}

	if (ws->header.opcode != WS_OPCODE_CONT) {
		ws->current_opcode = ws->header.opcode;
	}

	// The header is located at the start of the buffer, so an offset is
	// needed.
	return stream_ws_process_payload(ws, dst, size,
			ws->header.header_length);
}

static ssize_t stream_ws_read_ready(struct stream_ws* ws, void* dst,
		size_t size)
{
	size_t total_read = 0;

	while (true) {
		ssize_t n_read = stream_ws_read_frame(ws, dst, size);
		if (n_read == 0)
			break;

		if (n_read < 0) {
			if (errno == EAGAIN) {
				break;
			}
			return -1;
		}

		total_read += n_read;
		dst += n_read;
		size -= n_read;
	}

	return total_read;
}

static ssize_t stream_ws_read_handshake(struct stream_ws* ws, void* dst,
		size_t size)
{
	if (ws->read_index >= sizeof(ws->read_buffer)) {
		// This header is suspiciously long
		stream__remote_closed(&ws->base);
		return -1;
	}

	ws->read_buffer[ws->read_index] = '\0';

	char reply[512];
	ssize_t header_len = ws_handshake(reply, sizeof(reply),
			(const char*)ws->read_buffer);
	if (header_len < 0)
		return 0;

	ws->tcp_stream->cork = false;
	stream_send_first(ws->tcp_stream, rcbuf_from_mem(reply, strlen(reply)));

	ws->read_index -= header_len;
	memmove(ws->read_buffer, ws->read_buffer + header_len, ws->read_index);

	ws->ws_state = STREAM_WS_STATE_READY;
	return stream_ws_read_ready(ws, dst, size);
}

static ssize_t stream_ws_read(struct stream* self, void* dst, size_t size)
{
	struct stream_ws* ws = (struct stream_ws*)self;

	stream_ws_read_into_buffer(ws);
	if (self->state == STREAM_STATE_CLOSED)
		return 0;

	switch (ws->ws_state) {
	case STREAM_WS_STATE_HANDSHAKE:
		return stream_ws_read_handshake(ws, dst, size);
	case STREAM_WS_STATE_READY:
		return stream_ws_read_ready(ws, dst, size);
	}
	abort();
	return -1;
}

static int stream_ws_send(struct stream* self, struct rcbuf* payload,
                stream_req_fn on_done, void* userdata)
{
	struct stream_ws* ws = (struct stream_ws*)self;

	struct ws_frame_header head = {
		.fin = true,
		.opcode = WS_OPCODE_BIN,
		.payload_length = payload->size,
	};

	uint8_t raw_head[WS_HEADER_MIN_SIZE];
	int head_len = ws_write_frame_header(raw_head, &head);

	stream_write(ws->tcp_stream, &raw_head, head_len, NULL, NULL);
	return stream_send(ws->tcp_stream, payload, on_done, userdata);
}

static struct rcbuf* stream_ws_chained_exec(struct stream* tcp_stream,
		void* userdata)
{
	struct stream_ws_exec_ctx* ctx = userdata;
	struct stream_ws* ws = tcp_stream->userdata;

	struct rcbuf* buf = ctx->exec(&ws->base, ctx->userdata);

	struct vec out;
	vec_init(&out, WS_HEADER_MIN_SIZE + buf->size + 1);

	struct ws_frame_header head = {
		.fin = true,
		.opcode = WS_OPCODE_BIN,
		.payload_length = buf->size,
	};
	int head_len = ws_write_frame_header(out.data, &head);
	out.len += head_len;

	vec_append(&out, buf->payload, buf->size);
	return rcbuf_new(out.data, out.len);
}

static void stream_ws_exec_and_send(struct stream* self, stream_exec_fn exec,
		void* userdata)
{
	struct stream_ws* ws = (struct stream_ws*)self;

	struct stream_ws_exec_ctx* ctx = calloc(1, sizeof(*ctx));
	assert(ctx);

	ctx->exec = exec;
	ctx->userdata = userdata;

	stream_exec_and_send(ws->tcp_stream, stream_ws_chained_exec, ctx);
}

static void stream_ws_event(struct stream* self, enum stream_event event)
{
	struct stream_ws* ws = self->userdata;

	if (event == STREAM_EVENT_REMOTE_CLOSED) {
		ws->base.state = STREAM_STATE_CLOSED;
	}

	ws->base.on_event(&ws->base, event);
}

static struct stream_impl impl = {
	.close = stream_ws_close,
	.destroy = stream_ws_destroy,
	.read = stream_ws_read,
	.send = stream_ws_send,
	.exec_and_send = stream_ws_exec_and_send,
};

struct stream* stream_ws_new(int fd, stream_event_fn on_event, void* userdata)
{
	struct stream_ws *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.state = STREAM_STATE_NORMAL;
	self->base.impl = &impl;
	self->base.on_event = on_event;
	self->base.userdata = userdata;

	self->tcp_stream = stream_new(fd, stream_ws_event, self);
	if (!self->tcp_stream) {
		free(self);
		return NULL;
	}

	// Don't send anything until handshake is done:
	self->tcp_stream->cork = true;

	return &self->base;
}
