/*
 * Copyright (c) 2019 - 2022 Andri Yngvason
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
#include "common.h"
#include "logging.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#ifdef HAVE_LIBAVUTIL
#include <libavutil/avutil.h>
#endif

#define EXPORT __attribute__((visibility("default")))

static nvnc_log_fn log_fn = nvnc_default_logger;

#ifndef NDEBUG
static enum nvnc_log_level log_level = NVNC_LOG_DEBUG;
#else
static enum nvnc_log_level log_level = NVNC_LOG_WARNING;
#endif

static bool is_initialised = false;

static char* trim_left(char* str)
{
	while (isspace(*str))
		++str;
	return str;
}

static char* trim_right(char* str)
{
	char* end = str + strlen(str) - 1;
	while (str < end && isspace(*end))
		*end-- = '\0';
	return str;
}

static inline char* trim(char* str)
{
	return trim_right(trim_left(str));
}

static const char* log_level_to_string(enum nvnc_log_level level)
{
	switch (level) {
	case NVNC_LOG_PANIC: return "PANIC";
	case NVNC_LOG_ERROR: return "ERROR";
	case NVNC_LOG_WARNING: return "Warning";
	case NVNC_LOG_INFO: return "Info";
	case NVNC_LOG_DEBUG: return "DEBUG";
	case NVNC_LOG_TRACE: return "TRACE";
	}

	return "UNKNOWN";
}

static FILE* stream_for_log_level(enum nvnc_log_level level)
{
	switch (level) {
	case NVNC_LOG_PANIC: return stderr;
	case NVNC_LOG_ERROR: return stderr;
	case NVNC_LOG_WARNING: return stderr;
	case NVNC_LOG_INFO: return stdout;
	case NVNC_LOG_DEBUG: return stdout;
	case NVNC_LOG_TRACE: return stdout;
	}

	return stderr;
}

static void nvnc__vlog(const struct nvnc_log_data* meta, const char* fmt,
		va_list args)
{
	char message[1024];

	if (meta->level <= log_level) {
		vsnprintf(message, sizeof(message), fmt, args);
		log_fn(meta, trim(message));
	}

	if (meta->level == NVNC_LOG_PANIC)
		abort();
}

EXPORT
void nvnc_default_logger(const struct nvnc_log_data* meta,
		const char* message)
{
	const char* level = log_level_to_string(meta->level);
	FILE* stream = stream_for_log_level(meta->level);

	if (meta->level == NVNC_LOG_INFO)
		fprintf(stream, "Info: %s\n", message);
	else
		fprintf(stream, "%s: %s: %d: %s\n", level, meta->file,
				meta->line, message);

	fflush(stream);
}

#ifdef HAVE_LIBAVUTIL
static enum nvnc_log_level nvnc__log_level_from_av(int level)
{
	switch (level) {
	case AV_LOG_PANIC: return NVNC_LOG_PANIC;
	case AV_LOG_FATAL: return NVNC_LOG_ERROR;
	case AV_LOG_ERROR: return NVNC_LOG_ERROR;
	case AV_LOG_WARNING: return NVNC_LOG_WARNING;
	case AV_LOG_INFO: return NVNC_LOG_INFO;
	case AV_LOG_VERBOSE: return NVNC_LOG_INFO;
	case AV_LOG_DEBUG: return NVNC_LOG_DEBUG;
	case AV_LOG_TRACE: return NVNC_LOG_TRACE;
	}

	return NVNC_LOG_TRACE;
}

static int nvnc__log_level_to_av(enum nvnc_log_level level)
{
	switch (level) {
	case NVNC_LOG_PANIC: return AV_LOG_PANIC;
	case NVNC_LOG_ERROR: return AV_LOG_ERROR;
	case NVNC_LOG_WARNING: return AV_LOG_WARNING;
	case NVNC_LOG_INFO: return AV_LOG_INFO;
	case NVNC_LOG_DEBUG: return AV_LOG_DEBUG;
	case NVNC_LOG_TRACE: return AV_LOG_TRACE;
	}

	return AV_LOG_TRACE;
}

static void nvnc__av_log_callback(void* ptr, int level, const char* fmt,
		va_list va)
{
	struct nvnc_log_data meta = {
		.level = nvnc__log_level_from_av(level),
		.file = "libav",
		.line = 0,
	};
	nvnc__vlog(&meta, fmt, va);
}
#endif

EXPORT
void nvnc_set_log_level(enum nvnc_log_level level)
{
	log_level = level;

#ifdef HAVE_LIBAVUTIL
	av_log_set_level(nvnc__log_level_to_av(level));
#endif
}

EXPORT
void nvnc_set_log_fn(nvnc_log_fn fn)
{
	log_fn = fn;
}

EXPORT
void nvnc__log(const struct nvnc_log_data* meta,
		const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	nvnc__vlog(meta, fmt, ap);
	va_end(ap);
}

void nvnc__log_init(void)
{
	if (is_initialised)
		return;

#ifdef HAVE_LIBAVUTIL
	av_log_set_callback(nvnc__av_log_callback);
#endif

	is_initialised = true;
}
