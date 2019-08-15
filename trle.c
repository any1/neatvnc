#include <string.h>

#include "rfb-proto.h"

struct vnc_fb {
	uint8_t *data;
	uint32_t pixfmt;
	uint32_t width;
	uint32_t height;
};

void rfb_zrle_encode_tile(uint8_t *dst, uint32_t dst_pixfmt,
			  const struct vnc_fb *src)
{
	
}
