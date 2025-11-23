#include "compositor.h"
#include "transform-util.h"
#include "fb.h"

#include <aml.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <sixel.h>
#include <drm_fourcc.h>
#include <math.h>

struct nvnc_fb* read_png_file(const char *filename);

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

static void on_done(struct nvnc_composite_fb* cfb,
		struct pixman_region16* damage, void* userdata)
{
	aml_exit(aml_get_default());
	nvnc_fb_write_sixel(cfb->fbs[0]);
}

int main(int argc, char* argv[])
{
	int rc = 0;

	if (argc < 4) {
		fprintf(stderr, "Missing arguments\n");
		return 1;
	}

	struct aml* aml = aml_new();
	aml_set_default(aml);

	aml_require_workers(aml, -1);

	struct nvnc_composite_fb cfb = {};

	int x_pos = 0;
	for (int i = 1; i < argc; i += 3) {
		const char* file = argv[i];
		printf("Adding %s\n", file);

		enum nvnc_transform transform = atoi(argv[i + 1]);
		double scale = strtod(argv[i + 2], NULL);

		struct nvnc_fb* fb = read_png_file(file);
		if (!fb) {
			printf("Failed to read png file: %s\n", file);
			goto out;
		}

		fb->transform = transform;

		uint32_t transformed_width, transformed_height;
		transformed_width = fb->width;
		transformed_height = fb->height;
		nvnc_transform_dimensions(transform, &transformed_width,
				&transformed_height);

		fb->logical_width = scale > 0.0 ?
			round(transformed_width * scale) : fb->width;
		fb->logical_height = scale > 0.0 ?
			round(transformed_height * scale) : fb->height;

		fb->x_off = x_pos;
		x_pos += fb->logical_width;

		cfb.fbs[cfb.n_fbs++] = fb;
	}

	struct compositor* compositor = compositor_create();
	assert(compositor);

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0,
			nvnc_composite_fb_width(&cfb),
			nvnc_composite_fb_height(&cfb));
	
	compositor_feed(compositor, &cfb, &damage, on_done, NULL);

	aml_run(aml);

	printf("Done\n");

	compositor_destroy(compositor);
out:
	nvnc_composite_fb_unref(&cfb);
	aml_unref(aml);

	return rc;
}
