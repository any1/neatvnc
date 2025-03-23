/*
 * Copyright (c) 2025 Andri Yngvason
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

#include "parallel-deflate.h"
#include "vec.h"
#include "sys/queue.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <aml.h>
#include <zlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>

#define INPUT_BLOCK_SIZE (128 * 1024)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct output_chunk {
	uint32_t seq;
	void* data;
	size_t len;
	TAILQ_ENTRY(output_chunk) link;
};

TAILQ_HEAD(output_chunk_list, output_chunk);

struct parallel_deflate {
	int level, window_bits, mem_level, strategy;
	uint32_t seq;
	uint32_t start_seq;
	bool is_at_start;
	struct vec input;

	struct output_chunk_list output_chunks;
	pthread_mutex_t output_chunk_mutex;
	pthread_cond_t output_chunk_cond;
};

struct deflate_job {
	struct parallel_deflate* parent;
	uint32_t seq;
	z_stream zs;
	struct vec input, output;
};

static void output_chunk_list_lock(struct parallel_deflate* self)
{
	pthread_mutex_lock(&self->output_chunk_mutex);
}

static void output_chunk_list_unlock(struct parallel_deflate* self)
{
	pthread_mutex_unlock(&self->output_chunk_mutex);
}

struct parallel_deflate* parallel_deflate_new(int level, int window_bits,
		int mem_level, int strategy)
{
	struct parallel_deflate* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	assert(window_bits < 0);

	self->level = level;
	self->window_bits = window_bits;
	self->mem_level = mem_level;
	self->strategy = strategy;
	self->is_at_start = true;

	TAILQ_INIT(&self->output_chunks);
	pthread_mutex_init(&self->output_chunk_mutex, NULL);
	pthread_cond_init(&self->output_chunk_cond, NULL);

	vec_init(&self->input, INPUT_BLOCK_SIZE * 2);

	return self;
}

static int deflate_vec(struct vec* dst, const struct vec* src, z_stream* zs)
{
	zs->next_in = src->data;
	zs->avail_in = src->len;

	do {
		if (dst->len == dst->cap && vec_reserve(dst, dst->cap * 2) < 0)
			return -1;

		zs->next_out = ((Bytef*)dst->data) + dst->len;
		zs->avail_out = dst->cap - dst->len;

		int r = deflate(zs, Z_SYNC_FLUSH);
		if (r == Z_STREAM_ERROR)
			return -1;

		dst->len = zs->next_out - (Bytef*)dst->data;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}

static void insert_output_chunk(struct parallel_deflate* self,
		struct output_chunk* chunk)
{
	struct output_chunk *end = TAILQ_LAST(&self->output_chunks,
			output_chunk_list);
	while (end && end->seq > chunk->seq)
		end = TAILQ_PREV(end, output_chunk_list, link);

	if (end) {
		assert(end->seq != chunk->seq);
		TAILQ_INSERT_AFTER(&self->output_chunks, end, chunk, link);
	} else {
		TAILQ_INSERT_HEAD(&self->output_chunks, chunk, link);
	}

	pthread_cond_signal(&self->output_chunk_cond);
}

static bool consolidate_complete_chunk_segments(struct parallel_deflate* self,
		struct vec* out)
{
	bool have_end_chunk = false;

	struct output_chunk* chunk;
	while (!TAILQ_EMPTY(&self->output_chunks)) {
		chunk = TAILQ_FIRST(&self->output_chunks);
		if (chunk->seq != self->start_seq)
			break;

		self->start_seq++;

		TAILQ_REMOVE(&self->output_chunks, chunk, link);

		if (self->is_at_start) {
			uint8_t header[] = { 0x78, 0x01 };
			if (out)
				vec_append(out, header, sizeof(header));
			self->is_at_start = false;
		}

		if (out)
			vec_append(out, chunk->data, chunk->len);

		if (chunk->data == NULL)
			have_end_chunk = true;

		free(chunk->data);
		free(chunk);
	}

	return have_end_chunk;
}

static void do_work(struct aml_work* work)
{
	struct deflate_job* job = aml_get_userdata(work);
	struct parallel_deflate* self = job->parent;

	// TODO: Maintain 32K input window for dictionary
	deflate_vec(&job->output, &job->input, &job->zs);

	struct output_chunk* chunk = calloc(1, sizeof(*chunk));
	assert(chunk);
	chunk->seq = job->seq;
	chunk->data = job->output.data;
	chunk->len = job->output.len;
	memset(&job->output, 0, sizeof(job->output));

	output_chunk_list_lock(self);
	insert_output_chunk(self, chunk);
	output_chunk_list_unlock(self);
}

static void deflate_job_destroy(void* userdata)
{
	struct deflate_job* job = userdata;
	deflateEnd(&job->zs);
	vec_destroy(&job->output);
	vec_destroy(&job->input);
	free(job);
}

static void schedule_deflate_job(struct parallel_deflate* self,
		const void* input, size_t len)
{
	struct deflate_job* job = calloc(1, sizeof(*job));
	assert(job);

	job->parent = self;
	job->seq = self->seq++;
	int rc = deflateInit2(&job->zs, self->level, Z_DEFLATED,
			self->window_bits, self->mem_level, self->strategy);
	assert(rc == Z_OK);

	vec_init(&job->output, len);

	vec_init(&job->input, len);
	vec_append(&job->input, input, len);

	struct aml_work* work = aml_work_new(do_work, NULL, job,
			deflate_job_destroy);
	aml_start(aml_get_default(), work);
	aml_unref(work);
}

void parallel_deflate_feed(struct parallel_deflate* self, struct vec* out,
		const void* data, size_t len)
{
	vec_append(&self->input, data, len);

	size_t n_blocks = self->input.len / INPUT_BLOCK_SIZE;
	uint8_t* bytes = self->input.data;

	for (size_t i = 0; i < n_blocks; ++i)
		schedule_deflate_job(self, bytes + INPUT_BLOCK_SIZE * i,
				INPUT_BLOCK_SIZE);

	size_t processed = n_blocks * INPUT_BLOCK_SIZE;
	self->input.len -= processed;
	memmove(bytes, bytes + processed, self->input.len);
}

static void parallel_deflate_flush(struct parallel_deflate* self,
		struct vec* out)
{
	output_chunk_list_lock(self);

	struct output_chunk* end_chunk = calloc(1, sizeof(*end_chunk));
	assert(end_chunk);
	end_chunk->seq = self->seq++;
	insert_output_chunk(self, end_chunk);

	while (!consolidate_complete_chunk_segments(self, out))
		pthread_cond_wait(&self->output_chunk_cond,
				&self->output_chunk_mutex);
	output_chunk_list_unlock(self);
}

void parallel_deflate_sync(struct parallel_deflate* self, struct vec* out)
{
	if (self->input.len) {
		assert(self->input.len < INPUT_BLOCK_SIZE);
		schedule_deflate_job(self, self->input.data, self->input.len);
		vec_clear(&self->input);
	}

	parallel_deflate_flush(self, out);
}

void parallel_deflate_destroy(struct parallel_deflate* self)
{
	parallel_deflate_flush(self, NULL);
	vec_destroy(&self->input);
	pthread_mutex_destroy(&self->output_chunk_mutex);
	pthread_cond_destroy(&self->output_chunk_cond);
	free(self);
}
