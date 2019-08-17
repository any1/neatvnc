#ifndef _ZRLE_H_
#define _ZRLE_H_

#include <stdint.h>
#include <unistd.h>

struct rfb_pixel_format;

void pixel32_to_cpixel(uint8_t *restrict dst,
		       const struct rfb_pixel_format* dst_fmt,
		       const uint32_t *restrict src,
		       const struct rfb_pixel_format* src_fmt,
		       size_t bytes_per_cpixel, size_t len);

#endif /* _ZRLE_H_ */
