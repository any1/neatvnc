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

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "vec.h"
#include "http.h"

enum httplex_token_type {
	HTTPLEX_SOLIDUS,
	HTTPLEX_CR,
	HTTPLEX_LF,
	HTTPLEX_WS,
	HTTPLEX_LITERAL,
	HTTPLEX_KEY,
	HTTPLEX_VALUE,
	HTTPLEX_QUERY,
	HTTPLEX_AMPERSAND,
	HTTPLEX_EQ,
	HTTPLEX_END,
};

struct httplex_token {
	enum httplex_token_type type;
	const char* value;
};

enum httplex_state {
	HTTPLEX_STATE_REQUEST = 0,
	HTTPLEX_STATE_KEY,
	HTTPLEX_STATE_VALUE,
};

struct httplex {
	enum httplex_state state;
	struct httplex_token current_token;
	const char* input;
	const char* pos;
	const char* next_pos;
	struct vec buffer;
	int accepted;
	int errno_;
};

static int httplex_init(struct httplex* self, const char* input)
{
	memset(self, 0, sizeof(*self));

	self->input = input;
	self->pos = input;
	self->accepted = 1;

	if (vec_reserve(&self->buffer, 256) < 0)
		return -1;

	return 0;
}

static void httplex_destroy(struct httplex* self)
{
	vec_destroy(&self->buffer);
}

static inline int httplex__is_literal(char c)
{
	switch (c) {
	case '/': case '\r': case '\n': case ' ': case '\t':
	case '?': case '&': case '=':
		return 0;
	}

	return isprint(c);
}

static inline size_t httplex__literal_length(const char* str)
{
	size_t len = 0;
	while (httplex__is_literal(*str++))
		++len;
	return len;
}

static int httplex__classify_request_token(struct httplex* self)
{
	switch (*self->pos) {
	case '/':
		self->current_token.type = HTTPLEX_SOLIDUS;
		self->next_pos = self->pos + strspn(self->pos, "/");
		return 0;
	case '\r':
		self->current_token.type = HTTPLEX_CR;
		self->next_pos = self->pos + 1;
		return 0;
	case '\n':
		self->current_token.type = HTTPLEX_LF;
		self->next_pos = self->pos + 1;
		return 0;
	case '?':
		self->current_token.type = HTTPLEX_QUERY;
		self->next_pos = self->pos + 1;
		return 0;
	case '&':
		self->current_token.type = HTTPLEX_AMPERSAND;
		self->next_pos = self->pos + 1;
		return 0;
	case '=':
		self->current_token.type = HTTPLEX_EQ;
		self->next_pos = self->pos + 1;
		return 0;
	case ' ':
	case '\t':
		self->current_token.type = HTTPLEX_WS;
		self->next_pos = self->pos + strspn(self->pos, " \t");
		return 0;
	}

	if (httplex__is_literal(*self->pos)) {
		self->current_token.type = HTTPLEX_LITERAL;
		size_t len = httplex__literal_length(self->pos);
		self->next_pos = self->pos + len;
		vec_assign(&self->buffer, self->pos, len);
		vec_append(&self->buffer, "", 1);
		self->current_token.value = self->buffer.data;
		return 0;
	}

	return -1;
}

static inline int httplex__is_key_char(char c)
{
	return isalnum(c) || c == '-';
}

static inline size_t httplex__key_length(const char* str)
{
	size_t len = 0;
	while (httplex__is_key_char(*str++))
		++len;
	return len;
}

static int httplex__classify_key_token(struct httplex* self)
{
	switch (*self->pos) {
	case '\r':
		self->current_token.type = HTTPLEX_CR;
		self->next_pos = self->pos + 1;
		return 0;
	case '\n':
		self->current_token.type = HTTPLEX_LF;
		self->next_pos = self->pos + 1;
		return 0;
	}

	if (!httplex__is_key_char(*self->pos))
		return -1;

	size_t len = httplex__key_length(self->pos);

	if (self->pos[len] != ':')
		return -1;

	len += 1;

	self->next_pos = self->pos + len;
	self->next_pos += strspn(self->next_pos, " \t");

	vec_assign(&self->buffer, self->pos, len - 1);
	vec_append(&self->buffer, "", 1);

	self->current_token.type = HTTPLEX_KEY;
	self->current_token.value = self->buffer.data;
	return 0;
}

static int httplex__classify_value_token(struct httplex* self)
{
	size_t len = strcspn(self->pos, "\r");
	if (strncmp(&self->pos[len], "\r\n", 2) != 0)
		return -1;

	self->next_pos = self->pos + len + 2;

	vec_assign(&self->buffer, self->pos, len);
	vec_append(&self->buffer, "", 1);

	self->current_token.type = HTTPLEX_VALUE;
	self->current_token.value = self->buffer.data;
	return 0;
}

static int httplex__classify_token(struct httplex* self)
{
	switch (self->state) {
	case HTTPLEX_STATE_REQUEST:
		return httplex__classify_request_token(self);
	case HTTPLEX_STATE_KEY:
		return httplex__classify_key_token(self);
	case HTTPLEX_STATE_VALUE:
		return httplex__classify_value_token(self);
	};

	abort();
	return -1;
}

static struct httplex_token* httplex_next_token(struct httplex* self)
{
	if (self->current_token.type == HTTPLEX_END)
		return &self->current_token;

	if (!self->accepted)
		return &self->current_token;

	if (self->next_pos)
		self->pos = self->next_pos;

	if (httplex__classify_token(self) < 0)
		return NULL;

	self->accepted = 0;

	return &self->current_token;
}

static inline int httplex_accept_token(struct httplex* self)
{
	self->accepted = 1;
	return 1;
}

static int http__literal(struct httplex* lex, const char* str)
{
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_LITERAL)
		return 0;

	if (strcasecmp(str, tok->value) != 0)
		return 0;

	return httplex_accept_token(lex);
}

static int http__get(struct http_req* req, struct httplex* lex)
{
	if (!http__literal(lex, "GET"))
		return 0;

	req->method = HTTP_GET;
	return 1;
}

static int http__put(struct http_req* req, struct httplex* lex)
{
	if (!http__literal(lex, "PUT"))
		return 0;

	req->method = HTTP_PUT;
	return 1;
}

static int http__options(struct http_req* req, struct httplex* lex)
{
	if (!http__literal(lex, "OPTIONS"))
		return 0;

	req->method = HTTP_OPTIONS;
	return 1;
}

static int http__method(struct http_req* req, struct httplex* lex)
{
	return http__get(req, lex)
	    || http__put(req, lex)
	    || http__options(req, lex);
}

static int http__peek(struct httplex* lex, enum httplex_token_type type)
{
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != type)
		return 0;

	return 1;
}

static int http__expect(struct httplex* lex, enum httplex_token_type type)
{
	return http__peek(lex, type) && httplex_accept_token(lex);
}

static int http__version(struct httplex* lex)
{
	return http__literal(lex, "HTTP")
	    && http__expect(lex, HTTPLEX_SOLIDUS)
	    && http__literal(lex, "1.1");
}

static int http__url_path(struct http_req* req, struct httplex* lex)
{
	if (!http__expect(lex, HTTPLEX_SOLIDUS))
		return 0;

	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_LITERAL)
		return tok->type == HTTPLEX_WS;

	if (req->url_index >= URL_INDEX_MAX)
		return 0;

	char* elem = strdup(tok->value);
	if (!elem)
		return 0;

	req->url[req->url_index++] = elem;

	httplex_accept_token(lex);

	return http__peek(lex, HTTPLEX_SOLIDUS)
	     ? http__url_path(req, lex) : 1;
}

static int http__url_query_key(struct http_req* req, struct httplex* lex)
{
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_LITERAL)
		return 0;

	if (req->url_index >= URL_INDEX_MAX)
		return 0;

	char* elem = strdup(tok->value);
	if (!elem)
		return 0;

	req->url_query[req->url_query_index].key = elem;

	return httplex_accept_token(lex);
}

static int http__url_query_value(struct http_req* req, struct httplex* lex)
{
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_LITERAL)
		return 0;

	if (req->url_index >= URL_INDEX_MAX)
		return 0;

	char* elem = strdup(tok->value);
	if (!elem)
		return 0;

	req->url_query[req->url_query_index++].value = elem;

	return httplex_accept_token(lex);
}

static int http__url_query(struct http_req* req, struct httplex* lex)
{
	return http__url_query_key(req, lex)
	    && http__expect(lex, HTTPLEX_EQ)
	    && http__url_query_value(req, lex)
	    && http__expect(lex, HTTPLEX_AMPERSAND)
	       ? http__url_query(req, lex) : 1;
}

static int http__url(struct http_req* req, struct httplex* lex)
{
	return http__url_path(req, lex)
	    && http__expect(lex, HTTPLEX_QUERY) ? http__url_query(req, lex) : 1;
}

static int http__request(struct http_req* req, struct httplex* lex)
{
	return http__method(req, lex)
	    && http__expect(lex, HTTPLEX_WS)
	    && http__url(req, lex)
	    && http__expect(lex, HTTPLEX_WS)
	    && http__version(lex)
	    && http__expect(lex, HTTPLEX_CR)
	    && http__expect(lex, HTTPLEX_LF);
}

static int http__expect_key(struct httplex* lex, const char* key)
{
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_KEY)
		return 0;

	if (key && strcasecmp(tok->value, key) != 0)
		return 0;

	return httplex_accept_token(lex);
}

static int http__content_length(struct http_req* req, struct httplex* lex)
{
	lex->state = HTTPLEX_STATE_KEY;
	if (!http__expect_key(lex, "Content-Length"))
		return 0;

	lex->state = HTTPLEX_STATE_VALUE;
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_VALUE)
		return 0;

	req->content_length = atoi(tok->value);

	return httplex_accept_token(lex);
}

static int http__content_type(struct http_req* req, struct httplex* lex)
{
	lex->state = HTTPLEX_STATE_KEY;
	if (!http__expect_key(lex, "Content-Type"))
		return 0;

	lex->state = HTTPLEX_STATE_VALUE;
	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_VALUE)
		return 0;

	req->content_type = strdup(tok->value);

	return httplex_accept_token(lex);
}

static int http__field_key(struct http_req* req, struct httplex* lex)
{
	lex->state = HTTPLEX_STATE_KEY;

	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_KEY)
		return 0;

	req->field[req->field_index].key = strdup(tok->value);

	return httplex_accept_token(lex);
}

static int http__field_value(struct http_req* req, struct httplex* lex)
{
	lex->state = HTTPLEX_STATE_VALUE;

	struct httplex_token* tok = httplex_next_token(lex);
	if (!tok)
		return 0;

	if (tok->type != HTTPLEX_VALUE)
		return 0;

	req->field[req->field_index++].value = strdup(tok->value);

	return httplex_accept_token(lex);
}

static int http__field_kv(struct http_req* req, struct httplex* lex)
{
	return http__field_key(req, lex)
	    && http__field_value(req, lex);
}

static int http__header_kv(struct http_req* req, struct httplex* lex)
{
	return http__content_length(req, lex)
	    || http__content_type(req, lex)
	    || http__field_kv(req, lex);
}

static int http__header(struct http_req* req, struct httplex* lex)
{
	while (http__header_kv(req, lex));

	lex->state = HTTPLEX_STATE_KEY;
	if (http__expect(lex, HTTPLEX_CR))
		return http__expect(lex, HTTPLEX_LF);

	return 1;
}

int http_req_parse(struct http_req* req, const char* input)
{
	int rc = -1;
	memset(req, 0, sizeof(*req));

	struct httplex lex;
	if (httplex_init(&lex, input) < 0)
		return -1;

	if (!http__request(req, &lex))
		goto failure;

	if (!http__header(req, &lex))
		goto failure;

	req->header_length = lex.next_pos - input;

	rc = 0;
failure:
	httplex_destroy(&lex);
	return rc;
}

void http_req_free(struct http_req* req)
{
	free(req->content_type);

	for (size_t i = 0; i < req->url_index; ++i)
		free(req->url[i]);

	for (size_t i = 0; i < req->url_query_index; ++i) {
		free(req->url_query[i].key);
		free(req->url_query[i].value);
	}

	for (size_t i = 0; i < req->field_index; ++i) {
		free(req->field[i].key);
		free(req->field[i].value);
	}
}

const char* http_req_query(struct http_req* req, const char* key)
{
	for (size_t i = 0; i < req->url_query_index; ++i)
		if (strcmp(key, req->url_query[i].key) == 0)
			return req->url_query[i].value;

	return NULL;
}
