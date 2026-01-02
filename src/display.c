/*
 * Copyright (c) 2020 - 2025 Andri Yngvason
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
#include "fb.h"
#include "region.h"
#include "transform-util.h"
#include "enc/encoder.h"
#include "usdt.h"

#include <assert.h>
#include <stdlib.h>
#include <pixman.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT
struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos)
{
	struct nvnc_display* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	if (damage_refinery_init(&self->damage_refinery, 0, 0) < 0)
		goto refinery_failure;

	self->ref = 1;
	self->x_pos = x_pos;
	self->y_pos = y_pos;

	return self;

refinery_failure:
	free(self);

	return NULL;
}

static void nvnc__display_free(struct nvnc_display* self)
{
	if (self->buffer) {
		nvnc_fb_release(self->buffer);
		nvnc_fb_unref(self->buffer);
	}
	damage_refinery_destroy(&self->damage_refinery);
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
void nvnc_display_set_position(struct nvnc_display *self, uint16_t x,
		uint16_t y)
{
	if (x != self->x_pos || y != self->y_pos)
		nvnc__reset_encoders(self->server);

	self->x_pos = x;
	self->y_pos = y;
}

EXPORT
void nvnc_display_set_logical_size(struct nvnc_display *self, uint16_t width,
		uint16_t height)
{
	self->logical_width = width;
	self->logical_height = height;
}

EXPORT
struct nvnc* nvnc_display_get_server(const struct nvnc_display* self)
{
	return self->server;
}

EXPORT
void nvnc_display_feed_buffer(struct nvnc_display* self, struct nvnc_fb* fb,
		struct pixman_region16* damage)
{
	DTRACE_PROBE2(neatvnc, nvnc_display_feed_buffer, self, fb->pts);

	struct nvnc* server = self->server;
	assert(server);

	struct pixman_region16 refined_damage;
	pixman_region_init(&refined_damage);

	if (server->n_damage_clients != 0) {
		damage_refinery_resize(&self->damage_refinery, fb->width,
				fb->height);

		// TODO: Run the refinery in a worker thread?
		damage_refine(&self->damage_refinery, &refined_damage, damage, fb);
		damage = &refined_damage;
	} else {
		// Resizing to zero causes the damage refinery to be reset when
		// it's needed.
		damage_refinery_resize(&self->damage_refinery, 0, 0);
	}

	fb->x_off = self->x_pos;
	fb->y_off = self->y_pos;
	fb->logical_width = self->logical_width;
	fb->logical_height = self->logical_height;

	if (self->buffer) {
		nvnc_fb_release(self->buffer);
		nvnc_fb_unref(self->buffer);
	}

	self->buffer = fb;
	nvnc_fb_ref(fb);
	nvnc_fb_hold(fb);

	assert(self->server);

	// rotate
	struct pixman_region16 transformed_damage;
	pixman_region_init(&transformed_damage);
	nvnc_transform_region(&transformed_damage, damage, fb->transform,
			fb->width, fb->height);
	pixman_region_fini(&refined_damage);

	// scale
	double h_scale = 1.0, v_scale = 1.0;
	if (fb->logical_width && fb->logical_height) {
		uint32_t transformed_width = fb->width;
		uint32_t transformed_height = fb->height;
		nvnc_transform_dimensions(fb->transform, &transformed_width,
				&transformed_height);
		h_scale = (double)fb->logical_width / transformed_width;
		v_scale = (double)fb->logical_height / transformed_height;
	}

	struct pixman_region16 scaled_damage = { 0 };
	nvnc_region_scale(&scaled_damage, &transformed_damage,
			h_scale, v_scale);
	pixman_region_fini(&transformed_damage);

	// translate
	struct pixman_region16 shifted_damage = { 0 };
	nvnc_region_translate(&shifted_damage, &scaled_damage, fb->x_off,
			fb->y_off);
	pixman_region_fini(&scaled_damage);

	nvnc__damage_region(self->server, &shifted_damage);
	pixman_region_fini(&shifted_damage);
}
