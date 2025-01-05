#include "motion-estimator.h"
#include "damage-refinery.h"
#include "neatvnc.h"
#include "fb.h"
#include "pixels.h"

#include <aml.h>
#include <pixman.h>
#include <assert.h>
#include <sixel.h>
#include <stdio.h>
#include <drm_fourcc.h>
#include <tgmath.h>

#define ARROW_HEAD_LENGTH 10.0

struct nvnc_fb* read_png_file(const char *filename);

static inline uint32_t blend_colours(uint32_t src, uint32_t dst) {
	uint32_t src_a = (src >> 24) & 0xFF;
	uint32_t src_r = (src >> 16) & 0xFF;
	uint32_t src_g = (src >> 8) & 0xFF;
	uint32_t src_b = src & 0xFF;

	uint32_t dst_r = (dst >> 16) & 0xFF;
	uint32_t dst_g = (dst >> 8) & 0xFF;
	uint32_t dst_b = dst & 0xFF;

	// Alpha blend
	uint32_t inv_alpha = 255 - src_a;
	uint32_t r = (src_r * src_a + dst_r * inv_alpha) / 255;
	uint32_t g = (src_g * src_a + dst_g * inv_alpha) / 255;
	uint32_t b = (src_b * src_a + dst_b * inv_alpha) / 255;

	return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

static inline void draw_pixel(struct nvnc_fb* fb, int x, int y, uint32_t colour)
{
	assert(x >= 0 && x < fb->width && y >= 0);
	uint32_t* pixels = nvnc_fb_get_addr(fb);
	
	int pos = y * fb->stride + x;
	pixels[pos] = blend_colours(colour, pixels[pos]);
}

static void draw_line(struct nvnc_fb* fb, int x1, int y1, int x2, int y2,
		uint32_t colour)
{
	if (x1 == x2 && y1 == y2) {
		draw_pixel(fb, x1, y1, colour);
	} else if (x1 == x2) {
		if (y1 > y2) {
			int tmp = y1;
			y1 = y2;
			y2 = tmp;
		}
		for (int y = y1; y < y2; ++y)
			draw_pixel(fb, x1, y, colour);
	} else if (y1 == y2) {
		if (x1 > x2) {
			int tmp = x1;
			x1 = x2;
			x2 = tmp;
		}
		for (int x = x1; x < x2; ++x)
			draw_pixel(fb, x, y1, colour);
	} else {
		int dx = abs(x2 - x1);
		int dy = abs(y2 - y1);
		int sx = x1 < x2 ? 1 : -1;
		int sy = y1 < y2 ? 1 : -1;
		int err = (dx > dy ? dx : -dy) / 2;
		for (;;) {
			draw_pixel(fb, x1, y1, colour);
			if (x1 == x2 && y1 == y2)
				break;
			int e2 = err;
			if (e2 > -dx) {
				err -= dy;
				x1 += sx;
			}
			if (e2 < dy) {
				err += dx;
				y1 += sy;
			}
		}
	}
}

static void draw_arrow_head(struct nvnc_fb* fb,
		int x1, int y1, int x2, int y2, uint32_t colour) {
	double dx = x2 - x1;
	double dy = y2 - y1;
	double length = sqrt(dx * dx + dy * dy);

	assert(length != 0);

	dx /= length;
	dy /= length;

	const double angle = M_PI / 6.0;
	double cos_angle = cos(angle);
	double sin_angle = sin(angle);

	int head_x1 = x2 - ARROW_HEAD_LENGTH * (dx * cos_angle - dy * sin_angle);
	int head_y1 = y2 - ARROW_HEAD_LENGTH * (dx * sin_angle + dy * cos_angle);

	int head_x2 = x2 - ARROW_HEAD_LENGTH * (dx * cos_angle + dy * sin_angle);
	int head_y2 = y2 - ARROW_HEAD_LENGTH * (dy * sin_angle - dx * cos_angle);

	draw_line(fb, x2, y2, head_x1, head_y1, colour);
	draw_line(fb, x2, y2, head_x2, head_y2, colour);
}

static void draw_arrow(struct nvnc_fb* fb, int x1, int y1, int x2, int y2,
		uint32_t colour)
{
	draw_line(fb, x1, y1, x2, y2, colour);
	draw_arrow_head(fb, x1, y1, x2, y2, colour);
}

static void draw_block_grid(struct nvnc_fb* fb, uint32_t colour)
{
	for (int y = 0; y < fb->height; y += ME_BLOCK_SIZE)
		draw_line(fb, 0, y, fb->width, y, colour);

	for (int x = 0; x < fb->width; x += ME_BLOCK_SIZE)
		draw_line(fb, x, 0, x, fb->height, colour);
}

static int drm_fourcc_to_sixel_format(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_XRGB8888:
		return SIXEL_PIXELFORMAT_BGRA8888;
	case DRM_FORMAT_ARGB8888:
		return SIXEL_PIXELFORMAT_BGRA8888;
	case DRM_FORMAT_XBGR8888:
		return SIXEL_PIXELFORMAT_RGBA8888;
	case DRM_FORMAT_ABGR8888:
		return SIXEL_PIXELFORMAT_RGBA8888;
	case DRM_FORMAT_RGB565:
		return SIXEL_PIXELFORMAT_RGB565;
	case DRM_FORMAT_BGR565:
		return SIXEL_PIXELFORMAT_BGR565;
	default:
		return SIXEL_PIXELFORMAT_RGBA8888;
	}
}

static int nvnc_fb_write_sixel(struct nvnc_fb* fb)
{
	sixel_encoder_t* encoder = NULL;
	int rc = sixel_encoder_new(&encoder, NULL);
	assert(rc == SIXEL_OK);

	int sixel_format = drm_fourcc_to_sixel_format(fb->fourcc_format);

	sixel_frame_t* frame = NULL;
	rc = sixel_frame_new(&frame, NULL);
	assert(rc == SIXEL_OK);

	size_t buffer_size = fb->height * fb->stride *
		nvnc_fb_get_pixel_size(fb);
	void* buffer = malloc(buffer_size);
	memcpy(buffer, fb->addr, buffer_size);

	sixel_frame_init(frame, buffer, fb->width, fb->height, sixel_format,
			NULL, 256);

	int width = fb->width; //900;
	int height = (double)width / fb->width * fb->height;

	sixel_frame_resize(frame, width, height, RES_BILINEAR);

	rc = sixel_encoder_encode_bytes(encoder,
			sixel_frame_get_pixels(frame),
			sixel_frame_get_width(frame),
			sixel_frame_get_height(frame),
			sixel_frame_get_pixelformat(frame),
			sixel_frame_get_palette(frame),
			sixel_frame_get_ncolors(frame));

	sixel_frame_unref(frame);
	sixel_encoder_unref(encoder);

	printf("\n");
	return 0;
}

static void get_damage_region(struct pixman_region16* region,
		struct nvnc_fb* first, struct nvnc_fb* second)
{
	struct damage_refinery refinery = { 0 };
	damage_refinery_init(&refinery, first->width, first->height);

	struct pixman_region16 hint;
	pixman_region_init_rect(&hint, 0, 0, first->width, first->height);

	struct pixman_region16 dummy;
	pixman_region_init(&dummy);

	damage_refine(&refinery, &dummy, &hint, first);

	pixman_region_fini(&dummy);

	damage_refine(&refinery, region, &hint, second);

	pixman_region_fini(&hint);
}

static struct nvnc_fb* overlay_region(struct nvnc_fb* src,
		struct pixman_region16* region)
{
	struct nvnc_fb* dst = nvnc_fb_new(src->width, src->height,
			src->fourcc_format, src->width);

	bool ok;

	pixman_format_code_t dst_fmt = 0;
	ok = fourcc_to_pixman_fmt(&dst_fmt, dst->fourcc_format);
	assert(ok);

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(
			dst_fmt, dst->width, dst->height, dst->addr,
			nvnc_fb_get_pixel_size(dst) * dst->stride);

	pixman_format_code_t src_fmt = 0;
	ok = fourcc_to_pixman_fmt(&src_fmt, src->fourcc_format);
	assert(ok);

	pixman_image_t* srcimg = pixman_image_create_bits_no_clear(
			src_fmt, src->width, src->height, src->addr,
			nvnc_fb_get_pixel_size(src) * src->stride);

	pixman_image_composite(PIXMAN_OP_SRC, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			0, 0,
			dst->width, dst->height);

	pixman_image_unref(srcimg);

	pixman_color_t fill_color = {
		.red = 0xffff,
		.green = 0xffff,
		.blue = 0,
		.alpha = 0x7fff,
	};
	pixman_image_t* fill = pixman_image_create_solid_fill(&fill_color);

	int n_rects;
	struct pixman_box16* rects = pixman_region_rectangles(region, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		struct pixman_box16* rect = &rects[i];
		int x = rect->x1;
		int width = rect->x2 - x;
		int y = rect->y1;
		int height = rect->y2 - y;

		pixman_image_composite(PIXMAN_OP_OVER, fill, fill, dstimg,
				0, 0,
				0, 0,
				x, y,
				width, height);
	}

	pixman_image_unref(fill);
	pixman_image_unref(dstimg);
	return dst;
}

static int run_benchmark(const char* first_path, const char* second_path)
{
	struct nvnc_fb* first = read_png_file(first_path);
	struct nvnc_fb* second = read_png_file(second_path);

	if (!first || !second)
		return -1;

	assert(first->width == second->width && first->height == second->height);

	struct pixman_region16 damage;
	pixman_region_init(&damage);

	get_damage_region(&damage, first, second);

	struct nvnc_fb* visual = overlay_region(second, &damage);

	draw_block_grid(visual, 0x1fffffff);

	struct motion_estimator me = { 0 };
	motion_estimator_init(&me, first->width, first->height);
	motion_estimate(&me, first, NULL);
	motion_estimate(&me, second, &damage);

	for (uint32_t vy = 0; vy < me.vheight; ++vy)
		for (uint32_t vx = 0; vx < me.vwidth; ++vx) {
			struct motion_vector* v =
				&me.vectors[vx + vy * me.vwidth];
			if (!v->valid)
				continue;

			draw_arrow(visual,
					vx * ME_BLOCK_SIZE + ME_BLOCK_SIZE / 2,
					vy * ME_BLOCK_SIZE + ME_BLOCK_SIZE / 2,
					v->x + ME_BLOCK_SIZE / 2,
					v->y + ME_BLOCK_SIZE / 2,
					0xffffff00);
		}

	motion_estimator_deinit(&me);

	nvnc_fb_write_sixel(visual);
	nvnc_fb_unref(visual);
	pixman_region_fini(&damage);
	return 0;
}

int main(int argc, char *argv[])
{
	int rc = 0;

	char *first = "test-images/me-screen1.png";
	char *second = "test-images/me-screen2.png";

	struct aml* aml = aml_new();
	aml_set_default(aml);

	aml_require_workers(aml, -1);

	rc = run_benchmark(first, second) < 0 ? 1 : 0;

	aml_unref(aml);

	return rc;
}
