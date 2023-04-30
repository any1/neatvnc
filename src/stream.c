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

#include <assert.h>

int stream_close(struct stream* self)
{
	assert(self->impl && self->impl->close);
	return self->impl->close(self);
}

void stream_destroy(struct stream* self)
{
	assert(self->impl && self->impl->destroy);
	return self->impl->destroy(self);
}

int stream_send(struct stream* self, struct rcbuf* payload,
                stream_req_fn on_done, void* userdata)
{
	assert(self->impl && self->impl->send);
	return self->impl->send(self, payload, on_done, userdata);
}

int stream_send_first(struct stream* self, struct rcbuf* payload)
{
	assert(self->impl && self->impl->send);
	return self->impl->send_first(self, payload);
}

int stream_write(struct stream* self, const void* payload, size_t len,
                 stream_req_fn on_done, void* userdata)
{
	struct rcbuf* buf = rcbuf_from_mem(payload, len);
	return buf ? stream_send(self, buf, on_done, userdata) : -1;
}

ssize_t stream_read(struct stream* self, void* dst, size_t size)
{
	assert(self->impl && self->impl->read);
	return self->impl->read(self, dst, size);
}

void stream_exec_and_send(struct stream* self, stream_exec_fn exec_fn,
		void* userdata)
{
	assert(self->impl);
	if (self->impl->exec_and_send)
		self->impl->exec_and_send(self, exec_fn, userdata);
	else
		stream_send(self, exec_fn(self, userdata), NULL, NULL);
}

int stream_upgrade_to_tls(struct stream* self, void* context)
{
	assert(self->impl && self->impl->upgrade_to_tls);
	return self->impl->upgrade_to_tls(self, context);
}
