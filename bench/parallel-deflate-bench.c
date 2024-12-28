#include "parallel-deflate.h"
#include "vec.h"

#include <stdio.h>
#include <aml.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>

static int level = 1;
static int method = Z_DEFLATED;
static int window_bits = 15;
static int mem_level = 9;
static int strategy = Z_DEFAULT_STRATEGY;

struct stopwatch {
	uint64_t cpu;
	uint64_t real;
};

static uint64_t gettime_us(clockid_t clock)
{
	struct timespec ts = { 0 };
	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void stopwatch_start(struct stopwatch* self)
{
	self->real = gettime_us(CLOCK_MONOTONIC);
	self->cpu = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
}

static void stopwatch_stop(const struct stopwatch* self, const char* report)
{
	uint64_t real_stop = gettime_us(CLOCK_MONOTONIC);
	uint64_t cpu_stop = gettime_us(CLOCK_PROCESS_CPUTIME_ID);
	uint64_t dt_real = real_stop - self->real;
	uint64_t dt_cpu = cpu_stop - self->cpu;
	double cpu_util = (double)dt_cpu / dt_real;
	printf("%s took %"PRIu64" µs with %.0f%% CPU utilisation\n", report,
			dt_real, round(cpu_util * 100.0));
}

static int zlib_transform(int (*method)(z_stream*, int), struct vec* dst,
		const struct vec* src, z_stream* zs)
{
	zs->next_in = src->data;
	zs->avail_in = src->len;

	do {
		if (dst->len == dst->cap && vec_reserve(dst, dst->cap * 2) < 0)
			return -1;

		zs->next_out = ((Bytef*)dst->data) + dst->len;
		zs->avail_out = dst->cap - dst->len;

		int r = method(zs, Z_SYNC_FLUSH);
		if (r == Z_STREAM_ERROR)
			return -1;

		dst->len = zs->next_out - (Bytef*)dst->data;
	} while (zs->avail_out == 0);

	assert(zs->avail_in == 0);

	return 0;
}

static int deflate_vec(struct vec* dst, const struct vec* src, z_stream* zs)
{
	return zlib_transform(deflate, dst, src, zs);
}

static int inflate_vec(struct vec* dst, const struct vec* src, z_stream* zs)
{
	return zlib_transform(inflate, dst, src, zs);
}

static int read_file(struct vec* dst, const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("Failed open file");
		return -1;
	}

	char buffer[4096];

	for (;;) {
		int n_read = read(fd, buffer, sizeof(buffer));
		if (n_read <= 0)
			break;

		vec_append(dst, buffer, n_read);
	}

	close(fd);
	return 0;
}

static void establish_baseline(const struct vec* raw_data)
{
	struct vec compressed, decompressed;
	vec_init(&compressed, raw_data->len);
	vec_init(&decompressed, raw_data->len);

	z_stream deflate_zs = {};
	int rc = deflateInit2(&deflate_zs, level, method, window_bits,
			mem_level, strategy);
	assert(rc == Z_OK);

	z_stream inflate_zs = {};
	rc = inflateInit(&inflate_zs);
	assert(rc == Z_OK);

	struct stopwatch stopwatch;

	stopwatch_start(&stopwatch);
	deflate_vec(&compressed, raw_data, &deflate_zs);
	stopwatch_stop(&stopwatch, "Single threaded deflate");

	stopwatch_start(&stopwatch);
	inflate_vec(&decompressed, &compressed, &inflate_zs);
	stopwatch_stop(&stopwatch, "Single threaded inflate");

	assert(decompressed.len == raw_data->len);
	assert(memcmp(decompressed.data, raw_data->data, decompressed.len) == 0);

	printf("Single threaded compression: %.1f%%\n",
			100.0 * (1.0 - (double)compressed.len /
				(double)raw_data->len));

	inflateEnd(&inflate_zs);
	deflateEnd(&deflate_zs);

	vec_destroy(&decompressed);
	vec_destroy(&compressed);
}

static int run_benchmark(const char* file)
{
	struct vec raw_data;
	struct vec deflate_result;

	vec_init(&raw_data, 4096);
	vec_init(&deflate_result, 4096);

	if (read_file(&raw_data, file) < 0)
		return 1;

	establish_baseline(&raw_data);

	struct parallel_deflate* pd = parallel_deflate_new(level, -window_bits,
			mem_level, strategy);
	assert(pd);

	struct stopwatch stopwatch;
	stopwatch_start(&stopwatch);

	parallel_deflate_feed(pd, &deflate_result, raw_data.data, raw_data.len);
	parallel_deflate_sync(pd, &deflate_result);

	stopwatch_stop(&stopwatch, "Parallel deflate");

	const uint8_t* compressed_data = deflate_result.data;
	assert(deflate_result.len > 2 && compressed_data[0] == 0x78
			&& compressed_data[1] == 1);

	z_stream inflate_zs = {};
	int rc = inflateInit(&inflate_zs);
	assert(rc == Z_OK);

	struct vec decompressed;
	vec_init(&decompressed, raw_data.len);
	rc = inflate_vec(&decompressed, &deflate_result, &inflate_zs);

	assert(rc == 0);
	assert(decompressed.len == raw_data.len);
	assert(memcmp(decompressed.data, raw_data.data, decompressed.len) == 0);

	vec_clear(&deflate_result);
	parallel_deflate_feed(pd, &deflate_result, raw_data.data, raw_data.len);
	parallel_deflate_sync(pd, &deflate_result);
	vec_clear(&decompressed);
	rc = inflate_vec(&decompressed, &deflate_result, &inflate_zs);

	assert(rc == 0);
	assert(decompressed.len == raw_data.len);
	assert(memcmp(decompressed.data, raw_data.data, decompressed.len) == 0);

	vec_destroy(&decompressed);

	printf("Parallel compression: %.1f%%\n",
			100.0 * (1.0 - (double)deflate_result.len /
				(double)raw_data.len));

	parallel_deflate_destroy(pd);
	inflateEnd(&inflate_zs);
	vec_destroy(&raw_data);
	vec_destroy(&deflate_result);
	return 0;
}

int main(int argc, char* argv[])
{
	const char* file = argv[1];
	if (!file) {
		fprintf(stderr, "Missing input file\n");
		return 1;
	}

	struct aml* aml = aml_new();
	aml_set_default(aml);

	aml_require_workers(aml, -1);

	int rc = run_benchmark(file);

	aml_unref(aml);
	return rc;
}
