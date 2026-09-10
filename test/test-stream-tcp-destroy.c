/*
 * This test exercises the failure paths in on_connection() (src/server.c),
 * where a freshly created stream is torn down with stream_destroy() alone,
 * without stream_close() being called first. stream_tcp_destroy() used to
 * call stream_close() as part of its own cleanup, and stream_tcp_close()
 * in turn called stream_destroy() again to release the extra reference it
 * took for itself. Starting from a single reference, that second call to
 * stream_destroy() dropped the refcount to zero a second time and freed
 * the stream from inside stream_tcp_destroy() while the outer call was
 * still using it, leading to a use-after-free followed by a double free.
 *
 * Under a plain build this corruption is mostly silent, but it is caught
 * right away under AddressSanitizer.
 */

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include <aml.h>

#include "stream/stream.h"

static void on_event(struct stream* self, enum stream_event event)
{
}

int main(void)
{
	struct aml* loop = aml_new();
	assert(loop);
	aml_set_default(loop);

	int fds[2];
	int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	assert(rc == 0);

	struct stream* stream = stream_new(fds[0], on_event, NULL);
	assert(stream);

	/* Mirrors the buffer_failure/payload_failure paths in on_connection() */
	stream_destroy(stream);

	close(fds[1]);
	aml_unref(loop);

	return 0;
}
