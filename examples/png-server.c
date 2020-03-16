/*
 * Copyright (c) 2019 Andri Yngvason
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

#include <neatvnc.h>

#include <stdio.h>
#include <aml.h>
#include <signal.h>
#include <assert.h>
#include <pixman.h>

struct nvnc_fb* read_png_file(const char* filename);

void on_sigint()
{
	aml_exit(aml_get_default());
}

int main(int argc, char* argv[])
{
	const char* file = argv[1];

	if (!file) {
		printf("Missing argument\n");
		return 1;
	}

	struct nvnc_fb* fb = read_png_file(file);
	if (!fb) {
		printf("Failed to read png file\n");
		return 1;
	}

	struct aml* aml = aml_new(NULL, 0);
	aml_set_default(aml);

	struct nvnc* server = nvnc_open("127.0.0.1", 5900);

	int width = nvnc_fb_get_width(fb);
	int height = nvnc_fb_get_height(fb);
	uint32_t fourcc_format = nvnc_fb_get_fourcc_format(fb);

	nvnc_set_dimensions(server, width, height, fourcc_format);
	nvnc_set_name(server, file);

	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, nvnc_fb_get_width(fb),
	                        nvnc_fb_get_height(fb));
	nvnc_feed_frame(server, fb, &region);
	pixman_region_fini(&region);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml_get_default(), sig);
	aml_unref(sig);

	aml_run(aml);

	nvnc_close(server);
	nvnc_fb_unref(fb);
	aml_unref(aml);
}
