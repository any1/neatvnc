#pragma once

#include <unistd.h>
#include <stdint.h>

struct nvnc_fb {
	int ref;
	void* addr;
	size_t size;
	uint16_t width;
	uint16_t height;
	uint32_t fourcc_format;
	uint64_t fourcc_modifier;
};
