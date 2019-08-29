#ifndef _VNC_UTIL_H_
#define _VNC_UTIL_H_

#include <uv.h>
#include <unistd.h>

struct vnc_write_request {
	uv_write_t request;
	uv_write_cb on_done;
	uv_buf_t buffer;
};

int vnc__write(uv_stream_t *stream, const void *payload, size_t size,
               uv_write_cb on_done);

int rfb_pixfmt_from_fourcc(struct rfb_pixel_format *dst, uint32_t src);

#endif /* _VNC_UTIL_H_ */
