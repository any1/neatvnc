/*
 * Copyright (c) 2024 Andri Yngvason
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

#include "h264-encoder.h"
#include "neatvnc.h"
#include "fb.h"
#include "pixels.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <aml.h>
#include <dirent.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_UP(a, b) ((b) * UDIV_UP((a), (b)))
#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

#define N_SRC_BUFS 3
#define N_DST_BUFS 3

struct h264_encoder_v4l2m2m_dst_buf {
	struct v4l2_buffer buffer;
	struct v4l2_plane plane;
	void* payload;
};

struct h264_encoder_v4l2m2m_src_buf {
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[4];
	int fd;
	bool is_taken;
	struct nvnc_fb* fb;
};

struct h264_encoder_v4l2m2m {
	struct h264_encoder base;

	uint32_t width;
	uint32_t height;
	uint32_t format;
	int quality; // TODO: Can we affect the quality?

	char driver[16];

	int fd;
	struct aml_handler* handler;

	struct h264_encoder_v4l2m2m_src_buf src_bufs[N_SRC_BUFS];
	int src_buf_index;

	struct h264_encoder_v4l2m2m_dst_buf dst_bufs[N_DST_BUFS];
};

struct h264_encoder_impl h264_encoder_v4l2m2m_impl;

static int v4l2_qbuf(int fd, const struct v4l2_buffer* inbuf)
{
	assert(inbuf->length <= 4);
	struct v4l2_plane planes[4];
	struct v4l2_buffer outbuf;
	outbuf = *inbuf;
	memcpy(&planes, inbuf->m.planes, inbuf->length * sizeof(planes[0]));
	outbuf.m.planes = planes;
	return ioctl(fd, VIDIOC_QBUF, &outbuf);
}

static inline int v4l2_dqbuf(int fd, struct v4l2_buffer* buf)
{
	return ioctl(fd, VIDIOC_DQBUF, buf);
}

static struct h264_encoder_v4l2m2m_src_buf* take_src_buffer(
		struct h264_encoder_v4l2m2m* self)
{
	unsigned int count = 0;
	int i = self->src_buf_index;

	struct h264_encoder_v4l2m2m_src_buf* buffer;
	do {
		buffer = &self->src_bufs[i++];
		i %= ARRAY_LENGTH(self->src_bufs);
	} while (++count < ARRAY_LENGTH(self->src_bufs) && buffer->is_taken);

	if (buffer->is_taken)
		return NULL;

	self->src_buf_index = i;
	buffer->is_taken = true;

	return buffer;
}

static bool any_src_buf_is_taken(struct h264_encoder_v4l2m2m* self)
{
	bool result = false;
	for (unsigned int i = 0; i < ARRAY_LENGTH(self->src_bufs); ++i)
		if (self->src_bufs[i].is_taken)
			result = true;
	return result;
}

static int u32_cmp(const void* pa, const void* pb)
{
	const uint32_t *a = pa;
	const uint32_t *b = pb;
	return *a < *b ? -1 : *a > *b;
}

static size_t get_supported_formats(struct h264_encoder_v4l2m2m* self,
		uint32_t* formats, size_t max_len)
{
	size_t i = 0;
	for (;; ++i) {
		struct v4l2_fmtdesc desc = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		};
		int rc = ioctl(self->fd, VIDIOC_ENUM_FMT, &desc);
		if (rc < 0)
			break;

		nvnc_trace("Got pixel format: %s", desc.description);

		formats[i] = desc.pixelformat;
	}

	qsort(formats, i, sizeof(*formats), u32_cmp);

	return i;
}

static bool have_v4l2_format(const uint32_t* formats, size_t n_formats,
		uint32_t format)
{
	return bsearch(&format, formats, n_formats, sizeof(format), u32_cmp);
}

static uint32_t v4l2_format_from_drm(const uint32_t* formats,
		size_t n_formats, uint32_t drm_format)
{
#define TRY_FORMAT(f) \
	if (have_v4l2_format(formats, n_formats, f)) \
		return f

	switch (drm_format) {
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		TRY_FORMAT(V4L2_PIX_FMT_RGBX32);
		TRY_FORMAT(V4L2_PIX_FMT_RGBA32);
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		TRY_FORMAT(V4L2_PIX_FMT_XRGB32);
		TRY_FORMAT(V4L2_PIX_FMT_ARGB32);
		TRY_FORMAT(V4L2_PIX_FMT_RGB32);
		break;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		TRY_FORMAT(V4L2_PIX_FMT_XBGR32);
		TRY_FORMAT(V4L2_PIX_FMT_ABGR32);
		TRY_FORMAT(V4L2_PIX_FMT_BGR32);
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		TRY_FORMAT(V4L2_PIX_FMT_BGRX32);
		TRY_FORMAT(V4L2_PIX_FMT_BGRA32);
		break;
	// TODO: More formats
	}

	return 0;
#undef TRY_FORMAT
}

// This driver mixes up pixel formats...
static uint32_t v4l2_format_from_drm_bcm2835(const uint32_t* formats,
		size_t n_formats, uint32_t drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return V4L2_PIX_FMT_RGBA32;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		// TODO: This could also be ABGR, based on how this driver
		// behaves
		return V4L2_PIX_FMT_BGR32;
	}
	return 0;
}

static int set_src_fmt(struct h264_encoder_v4l2m2m* self)
{
	int rc;

	uint32_t supported_formats[256];
	size_t n_formats = get_supported_formats(self, supported_formats,
			ARRAY_LENGTH(supported_formats));

	uint32_t format;
	if (strcmp(self->driver, "bcm2835-codec") == 0)
		format = v4l2_format_from_drm_bcm2835(supported_formats,
				n_formats, self->format);
	else
		format = v4l2_format_from_drm(supported_formats, n_formats,
				self->format);
	if (!format) {
		nvnc_log(NVNC_LOG_DEBUG, "Failed to find a proper pixel format");
		return -1;
	}

	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};
	rc = ioctl(self->fd, VIDIOC_G_FMT, &fmt);
	if (rc < 0) {
		return -1;
	}

	struct v4l2_pix_format_mplane* pix_fmt = &fmt.fmt.pix_mp;
	pix_fmt->pixelformat = format;
	pix_fmt->width = ALIGN_UP(self->width, 16);
	pix_fmt->height = ALIGN_UP(self->height, 16);

	rc = ioctl(self->fd, VIDIOC_S_FMT, &fmt);
	if (rc < 0) {
		return -1;
	}

	return 0;
}

static int set_dst_fmt(struct h264_encoder_v4l2m2m* self)
{
	int rc;

	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	};
	rc = ioctl(self->fd, VIDIOC_G_FMT, &fmt);
	if (rc < 0) {
		return -1;
	}

	struct v4l2_pix_format_mplane* pix_fmt = &fmt.fmt.pix_mp;
	pix_fmt->pixelformat = V4L2_PIX_FMT_H264;
	pix_fmt->width = self->width;
	pix_fmt->height = self->height;

	rc = ioctl(self->fd, VIDIOC_S_FMT, &fmt);
	if (rc < 0) {
		return -1;
	}

	return 0;
}

static int alloc_dst_buffers(struct h264_encoder_v4l2m2m* self)
{
	int n_bufs = ARRAY_LENGTH(self->dst_bufs);
	int rc;

	struct v4l2_requestbuffers req = {
		.memory = V4L2_MEMORY_MMAP,
		.count = n_bufs,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	};
	rc = ioctl(self->fd, VIDIOC_REQBUFS, &req);
	if (rc < 0)
		return -1;

	for (unsigned int i = 0; i < req.count; ++i) {
		struct h264_encoder_v4l2m2m_dst_buf* buffer = &self->dst_bufs[i];
		struct v4l2_buffer* buf = &buffer->buffer;

		buf->index = i;
		buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf->memory = V4L2_MEMORY_MMAP;
		buf->length = 1;
		buf->m.planes = &buffer->plane;

		rc = ioctl(self->fd, VIDIOC_QUERYBUF, buf);
		if (rc < 0)
			return -1;

		buffer->payload = mmap(0, buffer->plane.length,
				PROT_READ | PROT_WRITE, MAP_SHARED, self->fd,
				buffer->plane.m.mem_offset);
		if (buffer->payload == MAP_FAILED) {
			nvnc_log(NVNC_LOG_ERROR, "Whoops, mapping failed: %m");
			return -1;
		}
	}

	return 0;
}

static void enqueue_dst_buffers(struct h264_encoder_v4l2m2m* self)
{
	for (unsigned int i = 0; i < ARRAY_LENGTH(self->dst_bufs); ++i) {
		int rc = v4l2_qbuf(self->fd, &self->dst_bufs[i].buffer);
		assert(rc >= 0);
	}
}

static void process_dst_bufs(struct h264_encoder_v4l2m2m* self)
{
	int rc;
	struct v4l2_plane plane = { 0 };
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.length = 1,
		.m.planes = &plane,
	};

	while (true) {
		rc = v4l2_dqbuf(self->fd, &buf);
		if (rc < 0)
			break;

		uint64_t pts = buf.timestamp.tv_sec * UINT64_C(1000000) +
			buf.timestamp.tv_usec;
		struct h264_encoder_v4l2m2m_dst_buf* dstbuf =
			&self->dst_bufs[buf.index];
		size_t size = buf.m.planes[0].bytesused;

		static uint64_t last_pts;
		if (last_pts && last_pts > pts) {
			nvnc_log(NVNC_LOG_ERROR, "pts - last_pts = %"PRIi64,
					(int64_t)pts - (int64_t)last_pts);
		}
		last_pts = pts;

		nvnc_trace("Encoded frame (index %d) at %"PRIu64" µs with size: %zu",
				buf.index, pts, size);

		self->base.on_packet_ready(dstbuf->payload, size, pts,
				self->base.userdata);

		v4l2_qbuf(self->fd, &buf);
	}
}

static void process_src_bufs(struct h264_encoder_v4l2m2m* self)
{
	int rc;
	struct v4l2_plane planes[4] = { 0 };
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_DMABUF,
		.length = 1,
		.m.planes = planes,
	};

	while (true) {
		rc = v4l2_dqbuf(self->fd, &buf);
		if (rc < 0)
			break;

		struct h264_encoder_v4l2m2m_src_buf* srcbuf =
			&self->src_bufs[buf.index];
		srcbuf->is_taken = false;

		// TODO: This assumes that there's only one fd
		close(srcbuf->planes[0].m.fd);

		nvnc_fb_unmap(srcbuf->fb);
		nvnc_fb_release(srcbuf->fb);
		nvnc_fb_unref(srcbuf->fb);
		srcbuf->fb = NULL;
	}
}

static void stream_off(struct h264_encoder_v4l2m2m* self)
{
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(self->fd, VIDIOC_STREAMOFF, &type);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(self->fd, VIDIOC_STREAMOFF, &type);
}

static void free_dst_buffers(struct h264_encoder_v4l2m2m* self)
{
	for (unsigned int i = 0; i < ARRAY_LENGTH(self->dst_bufs); ++i) {
		struct h264_encoder_v4l2m2m_dst_buf* buf = &self->dst_bufs[i];
		munmap(buf->payload, buf->plane.length);
	}
}

static int stream_on(struct h264_encoder_v4l2m2m* self)
{
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(self->fd, VIDIOC_STREAMON, &type);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	return ioctl(self->fd, VIDIOC_STREAMON, &type);
}

static int alloc_src_buffers(struct h264_encoder_v4l2m2m* self)
{
	int rc;

	struct v4l2_requestbuffers req = {
		.memory = V4L2_MEMORY_DMABUF,
		.count = N_SRC_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};
	rc = ioctl(self->fd, VIDIOC_REQBUFS, &req);
	if (rc < 0)
		return -1;

	for (int i = 0; i < N_SRC_BUFS; ++i) {
		struct h264_encoder_v4l2m2m_src_buf* buffer = &self->src_bufs[i];
		struct v4l2_buffer* buf = &buffer->buffer;

		buf->index = i;
		buf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf->memory = V4L2_MEMORY_DMABUF;
		buf->length = 1;
		buf->m.planes = buffer->planes;

		rc = ioctl(self->fd, VIDIOC_QUERYBUF, buf);
		if (rc < 0)
			return -1;
	}

	return 0;
}

static void force_key_frame(struct h264_encoder_v4l2m2m* self)
{
	struct v4l2_control ctrl = { 0 };
	ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
	ctrl.value = 0;
	ioctl(self->fd, VIDIOC_S_CTRL, &ctrl);
}

static void encode_buffer(struct h264_encoder_v4l2m2m* self,
		struct nvnc_fb* fb)
{
	struct h264_encoder_v4l2m2m_src_buf* srcbuf = take_src_buffer(self);
	if (!srcbuf) {
		nvnc_log(NVNC_LOG_ERROR, "Out of source buffers. Dropping frame...");
		return;
	}

	assert(!srcbuf->fb);

	nvnc_fb_ref(fb);
	nvnc_fb_hold(fb);

	/* For some reason the v4l2m2m h264 encoder in the Rapberry Pi 4 gets
	 * really glitchy unless the buffer is mapped first.
	 * This should probably be handled by the driver, but it's not.
	 */
	nvnc_fb_map(fb);

	srcbuf->fb = fb;

	struct gbm_bo* bo = nvnc_fb_get_gbm_bo(fb);

	int n_planes = gbm_bo_get_plane_count(bo);
	int fd = gbm_bo_get_fd(bo);
	uint32_t height = ALIGN_UP(gbm_bo_get_height(bo), 16);

	for (int i = 0; i < n_planes; ++i) {
		uint32_t stride = gbm_bo_get_stride_for_plane(bo, i);
		uint32_t offset = gbm_bo_get_offset(bo, i);
		uint32_t size = stride * height;

		srcbuf->buffer.m.planes[i].m.fd = fd;
		srcbuf->buffer.m.planes[i].bytesused = size;
		srcbuf->buffer.m.planes[i].length = size;
		srcbuf->buffer.m.planes[i].data_offset = offset;
	}

	srcbuf->buffer.timestamp.tv_sec = fb->pts / UINT64_C(1000000);
	srcbuf->buffer.timestamp.tv_usec = fb->pts % UINT64_C(1000000);

	if (self->base.next_frame_should_be_keyframe)
		force_key_frame(self);
	self->base.next_frame_should_be_keyframe = false;

	int rc = v4l2_qbuf(self->fd, &srcbuf->buffer);
	if (rc < 0) {
		nvnc_log(NVNC_LOG_PANIC, "Failed to enqueue buffer: %m");
	}
}

static void process_fd_events(void* handle)
{
	struct h264_encoder_v4l2m2m* self = aml_get_userdata(handle);
	process_dst_bufs(self);
}

static void h264_encoder_v4l2m2m_configure(struct h264_encoder_v4l2m2m* self)
{
	struct v4l2_control ctrl = { 0 };

	ctrl.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
	ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
	ioctl(self->fd, VIDIOC_S_CTRL, &ctrl);

	ctrl.id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
	ctrl.value = INT_MAX;
	ioctl(self->fd, VIDIOC_S_CTRL, &ctrl);

	ctrl.id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
	ctrl.value = V4L2_MPEG_VIDEO_BITRATE_MODE_CQ;
	ioctl(self->fd, VIDIOC_S_CTRL, &ctrl);

	ctrl.id = V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY;
	ctrl.value = self->quality;
	ioctl(self->fd, VIDIOC_S_CTRL, &ctrl);
}

static bool can_encode_to_h264(int fd)
{
	size_t i = 0;
	for (;; ++i) {
		struct v4l2_fmtdesc desc = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		};
		int rc = ioctl(fd, VIDIOC_ENUM_FMT, &desc);
		if (rc < 0)
			break;

		if (desc.pixelformat == V4L2_PIX_FMT_H264)
			return true;
	}
	return false;
}

static bool can_handle_frame_size(int fd, uint32_t width, uint32_t height)
{
	size_t i = 0;
	for (;; ++i) {
		struct v4l2_frmsizeenum size = {
			.index = i,
			.pixel_format = V4L2_PIX_FMT_H264,
		};
		int rc = ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size);
		if (rc < 0)
			break;

		switch (size.type) {
		case V4L2_FRMSIZE_TYPE_DISCRETE:
			if (size.discrete.width == width &&
					size.discrete.height == height)
				return true;
			break;
		case V4L2_FRMSIZE_TYPE_CONTINUOUS:
		case V4L2_FRMSIZE_TYPE_STEPWISE:
			if (size.stepwise.min_width <= width &&
					width <= size.stepwise.max_width &&
					size.stepwise.min_height <= height &&
					height <= size.stepwise.max_height &&
					(16 % size.stepwise.step_width) == 0 &&
					(16 % size.stepwise.step_height) == 0)
				return true;
			break;
		}
	}
	return false;
}

static bool is_device_capable(int fd, uint32_t width, uint32_t height)
{
	struct v4l2_capability cap = { 0 };
	int rc = ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (rc < 0)
		return false;

	uint32_t required_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	if ((cap.capabilities & required_caps) != required_caps)
		return false;

	if (!can_encode_to_h264(fd))
		return false;

	if (!can_handle_frame_size(fd, width, height))
		return false;

	return true;
}

static int find_capable_device(uint32_t width, uint32_t height)
{
	int fd = -1;
	DIR *dir = opendir("/dev");
	assert(dir);

	for (;;) {
		struct dirent* entry = readdir(dir);
		if (!entry)
			break;

		if (strncmp(entry->d_name, "video", 5) != 0)
			continue;

		char path[256];
		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}

		if (is_device_capable(fd, width, height)) {
			nvnc_log(NVNC_LOG_DEBUG, "Using v4l2m2m device: %s",
					path);
			break;
		}
		close(fd);
		fd = -1;
	}

	closedir(dir);
	return fd;
}

static struct h264_encoder* h264_encoder_v4l2m2m_create(uint32_t width,
		uint32_t height, uint32_t format, int quality)
{
	struct h264_encoder_v4l2m2m* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.impl = &h264_encoder_v4l2m2m_impl;
	self->fd = -1;
	self->width = width;
	self->height = height;
	self->format = format;
	self->quality = quality;

	self->fd = find_capable_device(width, height);
	if (self->fd < 0)
		goto failure;

	struct v4l2_capability cap = { 0 };
	ioctl(self->fd, VIDIOC_QUERYCAP, &cap);
	strncpy(self->driver, (const char*)cap.driver, sizeof(self->driver));

	if (set_src_fmt(self) < 0)
		goto failure;

	if (set_dst_fmt(self) < 0)
		goto failure;

	h264_encoder_v4l2m2m_configure(self);

	if (alloc_dst_buffers(self) < 0)
		goto failure;

	if (alloc_src_buffers(self) < 0)
		goto failure;

	enqueue_dst_buffers(self);

	if (stream_on(self) < 0)
		goto failure;

	int flags = fcntl(self->fd, F_GETFL);
	fcntl(self->fd, F_SETFL, flags | O_NONBLOCK);

	self->handler = aml_handler_new(self->fd, process_fd_events, self, NULL);
	aml_set_event_mask(self->handler, AML_EVENT_READ);

	if (aml_start(aml_get_default(), self->handler) < 0) {
		aml_unref(self->handler);
		goto failure;
	}

	return &self->base;

failure:
	if (self->fd >= 0)
		close(self->fd);
	return NULL;
}

static void claim_all_src_bufs(
		struct h264_encoder_v4l2m2m* self)
{
	for (;;) {
		 process_src_bufs(self);
		 if (!any_src_buf_is_taken(self))
			 break;
		 usleep(10000);
	}
}

static void h264_encoder_v4l2m2m_destroy(struct h264_encoder* base)
{
	struct h264_encoder_v4l2m2m* self = (struct h264_encoder_v4l2m2m*)base;
	claim_all_src_bufs(self);
	aml_stop(aml_get_default(), self->handler);
	aml_unref(self->handler);
	stream_off(self);
	free_dst_buffers(self);
	if (self->fd >= 0)
		close(self->fd);
	free(self);
}

static void h264_encoder_v4l2m2m_feed(struct h264_encoder* base,
		struct nvnc_fb* fb)
{
	struct h264_encoder_v4l2m2m* self = (struct h264_encoder_v4l2m2m*)base;
	process_src_bufs(self);
	encode_buffer(self, fb);
}

struct h264_encoder_impl h264_encoder_v4l2m2m_impl = {
	.create = h264_encoder_v4l2m2m_create,
	.destroy = h264_encoder_v4l2m2m_destroy,
	.feed = h264_encoder_v4l2m2m_feed,
};
