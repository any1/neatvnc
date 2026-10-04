/*
 * Copyright (c) 2026 Andri Yngvason
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

#include "audio.h"
#include "neatvnc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

size_t audio_format_sample_size(enum nvnc_audio_format format)
{
	switch (format) {
	case NVNC_AUDIO_FORMAT_U8:
	case NVNC_AUDIO_FORMAT_S8:
		return 1;
	case NVNC_AUDIO_FORMAT_U16:
	case NVNC_AUDIO_FORMAT_S16:
		return 2;
	case NVNC_AUDIO_FORMAT_U32:
	case NVNC_AUDIO_FORMAT_S32:
		return 4;
	default:
		break;
	}
	return 0;
}

void audio_config_set_channel_count(struct nvnc_audio_config* config,
		uint32_t channel_count)
{
	memset(config->positions, 0, sizeof(config->positions));
	config->channel_count = channel_count;

	switch (channel_count) {
	case 1:
		config->positions[0] = NVNC_AUDIO_CHANNEL_MONO;
		break;
	case 2:
		config->positions[0] = NVNC_AUDIO_CHANNEL_FL;
		config->positions[1] = NVNC_AUDIO_CHANNEL_FR;
		break;
	}
}

EXPORT
bool nvnc_audio_config_is_enabled(const struct nvnc_audio_config* config)
{
	return config->is_enabled;
}

EXPORT
uint32_t nvnc_audio_config_get_channel_count(
		const struct nvnc_audio_config* config)
{
	return config->channel_count;
}

EXPORT
uint32_t nvnc_audio_config_get_rate(const struct nvnc_audio_config* config)
{
	return config->rate;
}

EXPORT
enum nvnc_audio_format nvnc_audio_config_get_format(
		const struct nvnc_audio_config* config)
{
	return config->format;
}

EXPORT
const enum nvnc_audio_channel* nvnc_audio_config_get_positions(
		const struct nvnc_audio_config* config)
{
	return config->positions;
}
