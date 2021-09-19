/*
 * Copyright (c) 2020 - 2021 Andri Yngvason
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

#include "transform-util.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <pixman.h>

/* Note: This function yields the inverse pixman transform of the
 * nvnc_transform.
 */
void nvnc_transform_to_pixman_transform(pixman_transform_t* dst,
		enum nvnc_transform src, int width, int height)
{
#define F1 pixman_fixed_1
	switch (src) {
	case NVNC_TRANSFORM_NORMAL:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_180:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED:
		{
			pixman_transform_t t = {{
				{ -F1, 0, width * F1 },
				{ 0, F1, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_90:
		{
			pixman_transform_t t = {{
				{ 0, F1, 0 },
				{ F1, 0, 0 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_180:
		{
			pixman_transform_t t = {{
				{ F1, 0, 0 },
				{ 0, -F1, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	case NVNC_TRANSFORM_FLIPPED_270:
		{
			pixman_transform_t t = {{
				{ 0, -F1, width * F1 },
				{ -F1, 0, height * F1 },
				{ 0, 0, F1 },
			}};
			*dst = t;
		}
		return;
	}
#undef F1

	abort();
}

static bool is_transform_90_degrees(enum nvnc_transform transform)
{
	switch (transform) {
	case NVNC_TRANSFORM_90:
	case NVNC_TRANSFORM_270:
	case NVNC_TRANSFORM_FLIPPED_90:
	case NVNC_TRANSFORM_FLIPPED_270:
		return true;
	default:
		break;
	}

	return false;
}

void nvnc_transform_dimensions(enum nvnc_transform transform, uint32_t* width,
		uint32_t* height)
{
	if (is_transform_90_degrees(transform)) {
		uint32_t tmp = *width;
		*width = *height;
		*height = tmp;
	}
}
