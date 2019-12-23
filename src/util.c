/*
 * Copyright (c) 2019 Andri Yngvason
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

#include "util.h"

#include <uv.h>
#include <stdlib.h>
#include <unistd.h>

static void on_write_req_done(uv_write_t* req, int status)
{
	struct vnc_write_request* self = (struct vnc_write_request*)req;
	if (self->on_done)
		self->on_done(req, status);
	free(self);
}

int vnc__write2(uv_stream_t* stream, const void* payload, size_t size,
                uv_write_cb on_done, void* userdata)
{
	struct vnc_write_request* req = calloc(1, sizeof(*req));
	if (!req)
		return -1;

	req->buffer.base = (char*)payload;
	req->buffer.len = size;
	req->on_done = on_done;
	req->userdata = userdata;

	return uv_write(&req->request, stream, &req->buffer, 1,
	                on_write_req_done);
}

int vnc__write(uv_stream_t* stream, const void* payload, size_t size,
               uv_write_cb on_done)
{
	return vnc__write2(stream, payload, size, on_done, NULL);
}
