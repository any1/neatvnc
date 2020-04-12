/*
 * Copyright (c) 2020 Andri Yngvason
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

#include "display.h"
#include "neatvnc.h"
#include "common.h"

#include <assert.h>
#include <stdlib.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos)
{
	struct nvnc_display* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;
	self->x_pos = x_pos;
	self->y_pos = y_pos;

	return self;
}

void nvnc__display_free(struct nvnc_display* self)
{
	if (self->buffer)
		nvnc_fb_unref(self->buffer);
	free(self);
}

EXPORT
void nvnc_display_ref(struct nvnc_display* self)
{
	self->ref++;
}

EXPORT
void nvnc_display_unref(struct nvnc_display* self)
{
	if (--self->ref == 0)
		nvnc__display_free(self);
}

EXPORT
struct nvnc* nvnc_display_get_server(const struct nvnc_display* self)
{
	return self->server;
}

EXPORT
void nvnc_display_set_buffer(struct nvnc_display* self, struct nvnc_fb* fb)
{
	if (self->buffer)
		nvnc_fb_unref(self->buffer);

	self->buffer = fb;
	nvnc_fb_ref(fb);
}

EXPORT
void nvnc_display_set_render_fn(struct nvnc_display* self, nvnc_render_fn fn)
{
	self->render_fn = fn;
}

EXPORT
void nvnc_display_damage_region(struct nvnc_display* self,
                                const struct pixman_region16* region)
{
	// TODO: Shift according to display position
	assert(self->server);
	nvnc__damage_region(self->server, region);
}

EXPORT
void nvnc_display_damage_whole(struct nvnc_display* self)
{
	assert(self->server);

	uint16_t width = nvnc_fb_get_width(self->buffer);
	uint16_t height = nvnc_fb_get_height(self->buffer);

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, width, height);
	nvnc_display_damage_region(self, &damage);
	pixman_region_fini(&damage);
}
