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

#include "stream/stream.h"
#include "stream/common.h"

#include <stdlib.h>

void stream_init(struct stream* self)
{
	self->ref = 1;
}

void stream_req__finish(struct stream_req* req, enum stream_req_status status)
{
	if (req->on_done)
		req->on_done(req->userdata, status);

	// exec userdata is heap allocated
	if (req->exec && req->userdata)
		free(req->userdata);

	rcbuf_unref(req->payload);
	free(req);
}

void stream__remote_closed(struct stream* self)
{
	stream_close(self);

	if (self->on_event)
		self->on_event(self, STREAM_EVENT_REMOTE_CLOSED);
}
