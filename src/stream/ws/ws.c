/*
 * Copyright (c) 2023 - 2025 Andri Yngvason
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

#include "stream/stream.h"
#include "stream/common.h"
#include "stream/tcp.h"
#include "stream/websocket.h"
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
	stream_event_fn on_event;
	enum stream_ws_state ws_state;
	struct ws_frame_header header;
	enum ws_opcode current_opcode;
	size_t read_index;
	uint8_t read_buffer[4096]; // TODO: Is this a reasonable size?
};

static void stream_ws_read_into_buffer(struct stream_ws* ws)
{
	ssize_t n_read = stream_tcp_read(&ws->base,
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
		stream_tcp_send(&ws->base, rcbuf_from_mem(buf, reply_len),
				NULL, NULL);
	}

	int payload_len = MIN(ws->read_index - offset, ws->header.payload_length);

	// Feed back the (unmasked) payload:
	struct rcbuf* rcbuf = rcbuf_new(malloc(payload_len), payload_len);
	assert(rcbuf && rcbuf->payload);
	ws_copy_payload(&ws->header, rcbuf->payload, ws->read_buffer + offset,
			payload_len);
	stream_tcp_send(&ws->base, rcbuf, NULL, NULL);

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
		stream__remote_closed(&ws->base);
		return 0;
	case WS_OPCODE_TEXT:
		// This is unexpected, but let's just ignore it...
		stream_ws_advance_read_buffer(ws, SIZE_MAX, offset);
		return 0;
	case WS_OPCODE_BIN:
		return stream_ws_copy_payload(ws, dst, size, offset);
	case WS_OPCODE_CLOSE:
		stream__remote_closed(&ws->base);
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

	ws->base.cork = false;
	stream_tcp_send_first(&ws->base, rcbuf_from_mem(reply, strlen(reply)));

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

	stream_tcp_send(&ws->base, rcbuf_from_mem(&raw_head, head_len),
			NULL, NULL);
	return stream_tcp_send(&ws->base, payload, on_done, userdata);
}

static struct rcbuf* stream_ws_chained_exec(struct stream* tcp_stream,
		void* userdata)
{
	struct stream_ws* ws = (struct stream_ws*)tcp_stream;
	struct stream_ws_exec_ctx* ctx = userdata;

	struct rcbuf* buf = ctx->exec(&ws->base, ctx->userdata);

	// TODO: This also needs to be cleaned it it's left on the send queue
	// when the stream is destroyed.
	free(ctx->userdata);

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
	rcbuf_unref(buf);
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

	stream_tcp_exec_and_send(&ws->base, stream_ws_chained_exec, ctx);
}

static struct stream_impl impl = {
	.close = stream_tcp_close,
	.destroy = stream_tcp_destroy,
	.read = stream_ws_read,
	.send = stream_ws_send,
	.exec_and_send = stream_ws_exec_and_send,
};

struct stream* stream_ws_new(int fd, stream_event_fn on_event, void* userdata)
{
	struct stream_ws *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	stream_init(&self->base);

	stream_tcp_init(&self->base, fd, on_event, userdata);
	self->base.impl =  &impl;

	// Don't send anything until handshake is done:
	self->base.cork = true;

	return &self->base;
}
