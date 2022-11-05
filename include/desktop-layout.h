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

#pragma once

#include <stdint.h>

struct nvnc_display;
struct rfb_screen;

struct nvnc_display_layout {
	struct nvnc_display* display;
	uint32_t id;
	uint16_t x_pos, y_pos;
	uint16_t width, height;
};

struct nvnc_desktop_layout {
	uint16_t width, height;
	uint8_t n_display_layouts;
	struct nvnc_display_layout display_layouts[0];
};

void nvnc_display_layout_init(
		struct nvnc_display_layout* display, struct rfb_screen* screen);
