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

#include "display.h"
#include "neatvnc.h"
#include "common.h"
#include "fb.h"
#include "resampler.h"
#include "transform-util.h"
#include "enc/encoder.h"
#include "usdt.h"

#include <assert.h>
#include <stdlib.h>

#define EXPORT __attribute__((visibility("default")))

static void nvnc_display__on_resampler_done(struct nvnc_fb* fb,
		struct pixman_region16* damage, void* userdata)
{
	struct nvnc_display* self = userdata;

	DTRACE_PROBE2(neatvnc, nvnc_display__on_resampler_done, self, fb->pts);

	if (self->buffer) {
		nvnc_fb_release(self->buffer);
		nvnc_fb_unref(self->buffer);
	}

	self->buffer = fb;
	nvnc_fb_ref(fb);
	nvnc_fb_hold(fb);

	assert(self->server);

	// TODO: Shift according to display position
	nvnc__damage_region(self->server, damage);
}

EXPORT
struct nvnc_display* nvnc_display_new(uint16_t x_pos, uint16_t y_pos)
{
	struct nvnc_display* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->resampler = resampler_create();
	if (!self->resampler)
		goto resampler_failure;

	if (damage_refinery_init(&self->damage_refinery, 0, 0) < 0)
		goto refinery_failure;

	self->ref = 1;
	self->x_pos = x_pos;
	self->y_pos = y_pos;

	return self;

refinery_failure:
	resampler_destroy(self->resampler);
resampler_failure:
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
	resampler_destroy(self->resampler);
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

	struct pixman_region16 transformed_damage;
	pixman_region_init(&transformed_damage);
	nvnc_transform_region(&transformed_damage, damage, fb->transform,
			fb->width, fb->height);

	resampler_feed(self->resampler, fb, &transformed_damage,
			nvnc_display__on_resampler_done, self);

	pixman_region_fini(&transformed_damage);
	pixman_region_fini(&refined_damage);
}
