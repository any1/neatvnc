/* Copyright (c) 2014-2016, Marel
 * Copyright (c) 2023, Andri Yngvason
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

#define URL_INDEX_MAX 32
#define URL_QUERY_INDEX_MAX 32
#define HTTP_FIELD_INDEX_MAX 32

#include <stddef.h>

enum http_method {
	HTTP_GET = 1,
	HTTP_PUT = 2,
	HTTP_OPTIONS = 4,
};

struct http_kv {
	char* key;
	char* value;
};

struct http_req {
	enum http_method method;
	size_t header_length;
	size_t content_length;
	char* content_type;
	size_t url_index;
	char* url[URL_INDEX_MAX];
	size_t url_query_index;
	struct http_kv url_query[URL_QUERY_INDEX_MAX];
        size_t field_index;
        struct http_kv field[HTTP_FIELD_INDEX_MAX];
};

int http_req_parse(struct http_req* req, const char* head);
void http_req_free(struct http_req* req);

const char* http_req_query(struct http_req* req, const char* key);
