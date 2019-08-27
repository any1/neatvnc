#ifndef _ZRLE_H_
#define _ZRLE_H_

#include <stdint.h>
#include <unistd.h>

struct rfb_pixel_format;
struct pixman_region16;
struct vec;

void pixel32_to_cpixel(uint8_t *restrict dst,
		       const struct rfb_pixel_format* dst_fmt,
		       const uint32_t *restrict src,
		       const struct rfb_pixel_format* src_fmt,
		       size_t bytes_per_cpixel, size_t len);

int zrle_encode_frame(struct vec *dst,
		      const struct rfb_pixel_format *dst_fmt,
		      const uint8_t *src,
		      const struct rfb_pixel_format *src_fmt,
		      int width, int height,
		      struct pixman_region16 *region);

#endif /* _ZRLE_H_ */
