#include "fb.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

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

void nvnc__fb_free(struct nvnc_fb* fb)
{
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
