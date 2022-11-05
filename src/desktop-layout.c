/*
 * Copyright (c) 2023 Philipp Zabel
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

#include "desktop-layout.h"
#include "neatvnc.h"
#include "rfb-proto.h"

#define EXPORT __attribute__((visibility("default")))

void nvnc_display_layout_init(
		struct nvnc_display_layout* display, struct rfb_screen* screen)
{
	display->display = NULL;
	display->id = ntohl(screen->id);
	display->x_pos = ntohs(screen->x);
	display->y_pos = ntohs(screen->y);
	display->width = ntohs(screen->width);
	display->height = ntohs(screen->height);
}

EXPORT
uint16_t nvnc_desktop_layout_get_width(const struct nvnc_desktop_layout* layout)
{
	return layout->width;
}

EXPORT
uint16_t nvnc_desktop_layout_get_height(const struct nvnc_desktop_layout* layout)
{
	return layout->height;
}

EXPORT
uint8_t nvnc_desktop_layout_get_display_count(
		const struct nvnc_desktop_layout* layout)
{
	return layout->n_display_layouts;
}

EXPORT
uint16_t nvnc_desktop_layout_get_display_x_pos(
		const struct nvnc_desktop_layout* layout, uint8_t display_index)
{
	if (display_index >= layout->n_display_layouts)
		return 0;
	return layout->display_layouts[display_index].x_pos;
}

EXPORT
uint16_t nvnc_desktop_layout_get_display_y_pos(
		const struct nvnc_desktop_layout* layout, uint8_t display_index)
{
	if (display_index >= layout->n_display_layouts)
		return 0;
	return layout->display_layouts[display_index].y_pos;
}

EXPORT
uint16_t nvnc_desktop_layout_get_display_width(
		const struct nvnc_desktop_layout* layout, uint8_t display_index)
{
	if (display_index >= layout->n_display_layouts)
		return 0;
	return layout->display_layouts[display_index].width;
}

EXPORT
uint16_t nvnc_desktop_layout_get_display_height(
		const struct nvnc_desktop_layout* layout, uint8_t display_index)
{
	if (display_index >= layout->n_display_layouts)
		return 0;
	return layout->display_layouts[display_index].height;
}

EXPORT
struct nvnc_display* nvnc_desktop_layout_get_display(
		const struct nvnc_desktop_layout* layout, uint8_t display_index)
{
	if (display_index >= layout->n_display_layouts)
		return NULL;
	return layout->display_layouts[display_index].display;
}
