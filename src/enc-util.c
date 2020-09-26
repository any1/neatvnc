/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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

#include "enc-util.h"
#include "rfb-proto.h"
#include "vec.h"

#include <arpa/inet.h>

int encode_rect_count(struct vec* dst, uint32_t count)
{
	struct rfb_server_fb_update_msg msg = {
		.type = RFB_SERVER_TO_CLIENT_FRAMEBUFFER_UPDATE,
		.n_rects = htons(count),
	};

	return vec_append(dst, &msg, sizeof(msg));
}

int encode_rect_head(struct vec* dst, enum rfb_encodings encoding,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct rfb_server_fb_rect head = {
		.encoding = htonl(encoding),
		.x = htons(x),
		.y = htons(y),
		.width = htons(width),
		.height = htons(height),
	};

	return vec_append(dst, &head, sizeof(head));
}

uint32_t calc_bytes_per_cpixel(const struct rfb_pixel_format* fmt)
{
	return fmt->bits_per_pixel == 32 ? fmt->depth / 8
	                                 : fmt->bits_per_pixel / 8;
}
