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

#include "neatvnc.h"

#include <aml.h>

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext.h>

#define CURSOR_SIZE 32

struct video_player {
	struct aml_work* work;
	AVFrame* pending_frame;
	AVCodecContext* codec_ctx;

	AVFilterGraph* filter_graph;
	AVFilterContext* filter_in;
	AVFilterContext* filter_out;

	AVFormatContext* format_context;
	int video_stream_index;

	struct nvnc* server;
	struct nvnc_display* display;
	struct aml_ticker* ticker;

	struct gbm_device* gbm;
	int drm_fd;

	int client_count;
};

struct frame_context {
	AVFrame* av_frame;
	struct gbm_bo* bo;
};

static uint64_t gettime_us(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static int init_filters(struct video_player* self, AVFrame* first_frame)
{
	int rc;

	self->filter_graph = avfilter_graph_alloc();
	if (!self->filter_graph)
		return -1;

	/* buffersrc */
	self->filter_in = avfilter_graph_alloc_filter(self->filter_graph,
			avfilter_get_by_name("buffer"), "in");
	if (!self->filter_in)
		goto failure;

	AVBufferSrcParameters* params = av_buffersrc_parameters_alloc();
	if (!params)
		goto failure;

	params->format = AV_PIX_FMT_VAAPI;
	params->width = first_frame->width;
	params->height = first_frame->height;
	params->sample_aspect_ratio = (AVRational){1, 1};
	params->time_base = (AVRational){1, 1000000};
	params->hw_frames_ctx = first_frame->hw_frames_ctx;

	rc = av_buffersrc_parameters_set(self->filter_in, params);
	av_free(params);
	if (rc < 0)
		goto failure;

	rc = avfilter_init_dict(self->filter_in, NULL);
	if (rc < 0)
		goto failure;

	/* buffersink */
	rc = avfilter_graph_create_filter(&self->filter_out,
			avfilter_get_by_name("buffersink"), "out", NULL,
			NULL, self->filter_graph);
	if (rc != 0)
		goto failure;

	/* Parse filter chain */
	AVFilterInOut* inputs = avfilter_inout_alloc();
	if (!inputs)
		goto failure;

	inputs->name = av_strdup("in");
	inputs->filter_ctx = self->filter_in;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	AVFilterInOut* outputs = avfilter_inout_alloc();
	if (!outputs) {
		avfilter_inout_free(&inputs);
		goto failure;
	}

	outputs->name = av_strdup("out");
	outputs->filter_ctx = self->filter_out;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	rc = avfilter_graph_parse(self->filter_graph, "scale_vaapi=format=bgr0",
			outputs, inputs, NULL);
	if (rc != 0)
		goto failure;

	for (unsigned int i = 0; i < self->filter_graph->nb_filters; ++i) {
		self->filter_graph->filters[i]->hw_device_ctx =
			av_buffer_ref(self->codec_ctx->hw_device_ctx);
	}

	rc = avfilter_graph_config(self->filter_graph, NULL);
	if (rc != 0)
		goto failure;

	return 0;

failure:
	avfilter_graph_free(&self->filter_graph);
	self->filter_in = NULL;
	self->filter_out = NULL;
	return -1;
}

static int receive_decoded_frame(struct video_player* self)
{
	int rc;
	AVFrame* frame = av_frame_alloc();
	AVFrame* filtered_frame = av_frame_alloc();
	AVFrame* drm_frame = av_frame_alloc();
	if (!frame || !filtered_frame || !drm_frame) {
		rc = -1;
		goto out;
	}

	rc = avcodec_receive_frame(self->codec_ctx, frame);
	if (rc < 0)
		goto out;

	if (!self->filter_graph && init_filters(self, frame) < 0) {
		fprintf(stderr, "Failed to init filters\n");
		rc = -1;
		goto out;
	}

	rc = av_buffersrc_add_frame_flags(self->filter_in, frame,
			AV_BUFFERSRC_FLAG_KEEP_REF);
	if (rc < 0) {
		fprintf(stderr, "Error feeding filter\n");
		goto out;
	}

	rc = av_buffersink_get_frame(self->filter_out, filtered_frame);
	if (rc < 0)
		goto out;

	drm_frame->format = AV_PIX_FMT_DRM_PRIME;
	av_hwframe_map(drm_frame, filtered_frame, AV_HWFRAME_MAP_DIRECT);
	av_frame_copy_props(drm_frame, filtered_frame);

	if (self->pending_frame) {
		av_frame_unref(self->pending_frame);
		av_frame_free(&self->pending_frame);
	}
	self->pending_frame = av_frame_clone(drm_frame);

out:
	av_frame_free(&drm_frame);
	av_frame_free(&filtered_frame);
	av_frame_free(&frame);
	return rc;
}

static void read_next_packet(struct video_player* self, AVPacket* packet)
{
	while (true) {
		int rc = av_read_frame(self->format_context, packet);
		if (rc < 0) {
			printf("Rewinding...\n");
			av_seek_frame(self->format_context,
					self->video_stream_index,
					0, AVSEEK_FLAG_BACKWARD);
			continue;
		}

		if (packet->stream_index != self->video_stream_index) {
			av_packet_unref(packet);
			continue;
		}

		break;
	}
}

static struct gbm_bo* import_drm_frame_to_gbm(struct gbm_device* gbm,
		AVFrame* frame)
{
	AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor* )frame->data[0];
	assert(desc);
	assert(desc->nb_layers > 0);

	struct gbm_import_fd_modifier_data import_data = {
		.width = frame->width,
		.height = frame->height,
		.format = desc->layers[0].format,
		.num_fds = desc->nb_objects,
		.modifier = desc->objects[0].format_modifier,
	};

	for (int i = 0; i < desc->nb_objects; i++) {
		import_data.fds[i] = desc->objects[i].fd;
	}

	for (int i = 0; i < (int)desc->layers[0].nb_planes; i++) {
		AVDRMPlaneDescriptor* plane = &desc->layers[0].planes[i];
		import_data.strides[i] = plane->pitch;
		import_data.offsets[i] = plane->offset;
	}

	return gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &import_data, 0);
}

static void on_frame_context_cleanup(void* userdata)
{
	struct frame_context* ctx = userdata;
	av_frame_unref(ctx->av_frame);
	av_frame_free(&ctx->av_frame);
	gbm_bo_destroy(ctx->bo);
	free(ctx);
}

static void do_decode_work(struct aml_work* work)
{
	struct video_player* self = aml_get_userdata(work);

	AVPacket* packet = av_packet_alloc();

	while (!self->pending_frame) {
		read_next_packet(self, packet);

		int rc = avcodec_send_packet(self->codec_ctx, packet);
		av_packet_unref(packet);
		if (rc < 0) {
			fprintf(stderr, "Error sending packet to decoder\n");
			continue;
		}

		while (rc >= 0)
			rc = receive_decoded_frame(self);

		if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
			fprintf(stderr, "Fatal decoding error\n");
			break;
		}
	}

	av_packet_free(&packet);
}

static void on_decode_done(struct aml_work* work)
{
	struct video_player* self = aml_get_userdata(work);

	AVFrame* frame = self->pending_frame;
	if (!frame)
		return;
	self->pending_frame = NULL;

	struct gbm_bo* bo = import_drm_frame_to_gbm(self->gbm, frame);
	if (!bo) {
		fprintf(stderr, "Failed to import DRM frame to GBM BO\n");
		av_frame_unref(frame);
		av_frame_free(&frame);
		return;
	}

	struct nvnc_frame* fb = nvnc_frame_from_gbm_bo(bo);
	assert(fb);

	struct frame_context* ctx = calloc(1, sizeof(*ctx));
	assert(ctx);
	ctx->av_frame = frame;
	ctx->bo = bo;

	struct nvnc_buffer* buffer = nvnc_frame_get_buffer(fb);
	nvnc_buffer_set_userdata(buffer, ctx, on_frame_context_cleanup);

	nvnc_display_feed_frame(self->display, fb);
	nvnc_frame_unref(fb);
}

static void hue_to_rgb(float h, uint8_t* r, uint8_t* g, uint8_t* b)
{
	float hp = h*  6.0f;
	float rf = fabsf(hp - 3.0f) - 1.0f;
	float gf = 2.0f - fabsf(hp - 2.0f);
	float bf = 2.0f - fabsf(hp - 4.0f);

	*r = (uint8_t)(fminf(fmaxf(rf, 0.0f), 1.0f) * 255.0f + 0.5f);
	*g = (uint8_t)(fminf(fmaxf(gf, 0.0f), 1.0f) * 255.0f + 0.5f);
	*b = (uint8_t)(fminf(fmaxf(bf, 0.0f), 1.0f) * 255.0f + 0.5f);
}

static struct nvnc_frame* create_cursor(float hue_offset)
{
	struct nvnc_frame* fb = nvnc_frame_new(CURSOR_SIZE, CURSOR_SIZE,
			DRM_FORMAT_RGBA8888, CURSOR_SIZE);
	assert(fb);

	uint32_t* pixels = nvnc_frame_get_addr(fb);
	float cx = (CURSOR_SIZE - 1) / 2.0f;
	float cy = (CURSOR_SIZE - 1) / 2.0f;
	float radius = CURSOR_SIZE / 2.0f;

	for (int i = 0; i < CURSOR_SIZE * CURSOR_SIZE; ++i) {
		int x = i % CURSOR_SIZE;
		int y = i / CURSOR_SIZE;

		float x_off = x - cx;
		float y_off = y - cy;
		float dist = sqrtf(x_off * x_off + y_off * y_off);

		if (dist > radius) {
			pixels[i] = 0;
			continue;
		}

		float hue = atan2f(y_off, x_off) / (2.0f * (float)M_PI);
		if (hue < 0.0f)
			hue += 1.0f;
		hue = fmodf(hue + hue_offset, 1.0f);

		uint8_t r, g, b;
		hue_to_rgb(hue, &r, &g, &b);

		pixels[i] = (r << 24) | (g << 16) | (b << 8) | 0xff;
	}

	return fb;
}

static void update_cursor(struct video_player* player)
{
	uint32_t period = 1000000; // µs
	uint32_t t_us = gettime_us() % period;
	float t_s = (float)t_us / period;

	struct nvnc_frame* cursor = create_cursor(t_s);
	assert(cursor);
	nvnc_set_cursor(player->server, cursor, CURSOR_SIZE / 2,
			CURSOR_SIZE / 2, true);
	nvnc_frame_unref(cursor);
}

static void on_tick(struct aml_ticker* handler)
{
	struct video_player* player = aml_get_userdata(handler);
	aml_start(aml_get_default(), player->work);
	update_cursor(player);
}

static void start_playback(struct video_player* player)
{
	printf("Playing...\n");
	aml_start(aml_get_default(), player->ticker);
}

static void pause_playback(struct video_player* player)
{
	printf("Paused...\n");
	aml_stop(aml_get_default(), player->ticker);
}

static void on_client_cleanup(struct nvnc_client* client)
{
	struct video_player* player =
		nvnc_get_userdata(nvnc_client_get_server(client));
	if (--player->client_count == 0)
		pause_playback(player);
}

static void on_new_client(struct nvnc_client* client)
{
	struct video_player* player =
		nvnc_get_userdata(nvnc_client_get_server(client));
	nvnc_set_client_cleanup_fn(client, on_client_cleanup);

	if (player->client_count++ == 0)
		start_playback(player);
}

static void on_sigint()
{
	aml_exit(aml_get_default());
}

static bool setup_gbm(struct video_player* player)
{
	player->drm_fd = open("/dev/dri/renderD128", O_RDWR);
	if (player->drm_fd < 0) {
		fprintf(stderr, "Failed to open render node: %m\n");
		return false;
	}

	player->gbm = gbm_create_device(player->drm_fd);
	if (!player->gbm) {
		fprintf(stderr, "Failed to create GBM device\n");
		close(player->drm_fd);
		player->drm_fd = -1;
		return false;
	}

	return true;
}

static bool setup_video(struct video_player* player, const char* filename)
{
	int rc = avformat_open_input(&player->format_context, filename, NULL,
			NULL);
	if (rc < 0) {
		fprintf(stderr, "Failed to open file: %s\n", filename);
		return false;
	}

	rc = avformat_find_stream_info(player->format_context, NULL);
	if (rc < 0) {
		fprintf(stderr, "Failed to find stream info\n");
		return false;
	}

	player->video_stream_index = av_find_best_stream(player->format_context,
			AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (player->video_stream_index < 0) {
		fprintf(stderr, "No video stream found\n");
		return false;
	}

	AVStream* stream =
		player->format_context->streams[player->video_stream_index];
	AVCodecParameters* par = stream->codecpar;

	av_dump_format(player->format_context, 0, filename, 0);

	const AVCodec* codec = avcodec_find_decoder(par->codec_id);
	if (!codec) {
		fprintf(stderr, "Failed to find decoder\n");
		return false;
	}

	player->codec_ctx = avcodec_alloc_context3(codec);
	if (!player->codec_ctx) {
		fprintf(stderr, "Failed to allocate codec context\n");
		return false;
	}

	avcodec_parameters_to_context(player->codec_ctx, par);

	AVBufferRef* hwctx_ref;
	rc = av_hwdevice_ctx_create(&hwctx_ref, AV_HWDEVICE_TYPE_VAAPI,
			NULL, NULL, 0);
	if (rc < 0) {
		fprintf(stderr, "Failed to create VAAPI device\n");
		return false;
	}
	player->codec_ctx->hw_device_ctx = hwctx_ref;

	rc = avcodec_open2(player->codec_ctx, codec, NULL);
	if (rc < 0) {
		fprintf(stderr, "Failed to open codec\n");
		return false;
	}

	player->work = aml_work_new(do_decode_work, on_decode_done, player,
			NULL);
	if (!player->work) {
		fprintf(stderr, "Failed to create decode work\n");
		return false;
	}

	AVRational fr = stream->avg_frame_rate;
	if (fr.num == 0 || fr.den == 0)
		fr = stream->r_frame_rate;

	uint64_t tick_period_us = (uint64_t)1000000 * fr.den / fr.num;

	player->ticker = aml_ticker_new(tick_period_us, on_tick, player, NULL);
	assert(player->ticker);

	return true;
}

static int usage(void)
{
	printf("Usage: video-player <video-file> [address [port]]\n");
	return 1;
}

int main(int argc, char* argv[])
{
	if (argc < 2)
		return usage();

	const char* filename = argv[1];
	const char* addr = argc > 2 ? argv[2] : "localhost";
	int port = argc > 3 ? atoi(argv[3]) : 5900;

	struct video_player player = {
		.drm_fd = -1,
	};

	struct aml* aml = aml_new();
	assert(aml);
	aml_set_default(aml);

	if (!setup_gbm(&player))
		goto out;

	if (!setup_video(&player, filename))
		goto out;

	player.server = nvnc_new();
	assert(player.server);

	nvnc_set_userdata(player.server, &player, NULL);

	int rc = nvnc_listen_tcp(player.server, addr, port, NVNC_STREAM_NORMAL);
	if (rc < 0) {
		fprintf(stderr, "Failed to listen on %s:%d\n", addr, port);
		goto out;
	}

	player.display = nvnc_display_new(0, 0);
	assert(player.display);

	nvnc_add_display(player.server, player.display);
	nvnc_set_name(player.server, filename);
	nvnc_set_new_client_fn(player.server, on_new_client);

	update_cursor(&player);

	aml_start(aml_get_default(), player.work);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml_get_default(), sig);
	aml_unref(sig);

	aml_run(aml);

	aml_stop(aml_get_default(), player.ticker);
	nvnc_del(player.server);
	nvnc_display_unref(player.display);

out:
	if (player.work) {
		aml_stop(aml_get_default(), player.work);
		aml_unref(player.work);
	}
	if (player.pending_frame) {
		av_frame_unref(player.pending_frame);
		av_frame_free(&player.pending_frame);
	}
	avfilter_graph_free(&player.filter_graph);
	avcodec_free_context(&player.codec_ctx);
	if (player.format_context)
		avformat_close_input(&player.format_context);
	if (player.ticker)
		aml_unref(player.ticker);
	if (player.gbm)
		gbm_device_destroy(player.gbm);
	if (player.drm_fd >= 0)
		close(player.drm_fd);
	aml_unref(aml);

	return 0;
}
