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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define EXPORT __attribute__((visibility("default")))

static void default_logger(const struct nvnc_log_data* meta,
		const char* message);

static nvnc_log_fn log_fn = default_logger;

#ifndef NDEBUG
static enum nvnc_log_level log_level = NVNC_LOG_DEBUG;
#else
static enum nvnc_log_level log_level = NVNC_LOG_WARNING;
#endif

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
		log_fn(meta, message);
	}

	if (meta->level == NVNC_LOG_PANIC)
		abort();
}

static void default_logger(const struct nvnc_log_data* meta,
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

EXPORT
void nvnc_set_log_level(enum nvnc_log_level level)
{
	log_level = level;
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
