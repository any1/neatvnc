#include "util.h"

#include <uv.h>
#include <stdlib.h>
#include <unistd.h>

static void on_write_req_done(uv_write_t *req, int status)
{
	struct vnc_write_request *self = (struct vnc_write_request*)req;
	if (self->on_done)
		self->on_done(req, status);
	free(self);
}

int vnc__write(uv_stream_t *stream, const void *payload, size_t size,
	       uv_write_cb on_done)
{
	struct vnc_write_request *req = calloc(1, sizeof(*req));
	if (!req)
		return -1;

	req->buffer.base = (char*)payload;
	req->buffer.len = size;
	req->on_done = on_done;

	return uv_write(&req->request, stream, &req->buffer, 1,
			on_write_req_done);
}
