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

#include "fb.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdatomic.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_UP(n, a) (UDIV_UP(n, a) * a)
#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_fb* nvnc_fb_new(uint16_t width, uint16_t height,
                            uint32_t fourcc_format)
{
	struct nvnc_fb* fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;

	fb->ref = 1;
	fb->width = width;
	fb->height = height;
	fb->fourcc_format = fourcc_format;
	fb->size = width * height * 4; /* Assume 4 byte format for now */

	size_t alignment = MAX(4, sizeof(void*));
	size_t aligned_size = ALIGN_UP(fb->size, alignment);

	/* fb could be allocated in single allocation, but I want to reserve
	 * the possiblity to create an fb with a pixel buffer passed from the
	 * user.
	 */
	fb->addr = aligned_alloc(alignment, aligned_size);
	if (!fb->addr) {
		free(fb);
		fb = NULL;
	}

	return fb;
}

EXPORT
enum nvnc_fb_flags nvnc_fb_get_flags(const struct nvnc_fb* fb)
{
	return fb->flags;
}

EXPORT
void nvnc_fb_set_flags(struct nvnc_fb* fb, enum nvnc_fb_flags flags)
{
	fb->flags = flags;
}

EXPORT
void* nvnc_fb_get_addr(const struct nvnc_fb* fb)
{
	return fb->addr;
}

EXPORT
uint16_t nvnc_fb_get_width(const struct nvnc_fb* fb)
{
	return fb->width;
}

EXPORT
uint16_t nvnc_fb_get_height(const struct nvnc_fb* fb)
{
	return fb->height;
}

EXPORT
uint32_t nvnc_fb_get_fourcc_format(const struct nvnc_fb* fb)
{
	return fb->fourcc_format;
}

static void nvnc__fb_free(struct nvnc_fb* fb)
{
	nvnc_cleanup_fn cleanup = fb->common.cleanup_fn;
	if (cleanup)
		cleanup(fb->common.userdata);

	free(fb->addr);
	free(fb);
}

EXPORT
void nvnc_fb_ref(struct nvnc_fb* fb)
{
	fb->ref++;
}

EXPORT
void nvnc_fb_unref(struct nvnc_fb* fb)
{
	if (--fb->ref == 0)
		nvnc__fb_free(fb);
}

EXPORT
void nvnc_fb_set_release_fn(struct nvnc_fb* fb, nvnc_fb_release_fn fn, void* context)
{
	fb->on_release = fn;
	fb->release_context = context;
}

void nvnc_fb_hold(struct nvnc_fb* fb)
{
	fb->hold_count++;
}

void nvnc_fb_release(struct nvnc_fb* fb)
{
	if (--fb->hold_count == 0 && fb->on_release)
		fb->on_release(fb, fb->release_context);
}
