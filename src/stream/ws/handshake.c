/*
 * Copyright (c) 2023 Andri Yngvason
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

#include "stream/websocket.h"
#include "stream/http.h"
#include "crypto.h"
#include "base64.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

static const char magic_uuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static void tolower_and_remove_ws(char* dst, const char* src)
{
	while (*src)
		if (!isspace(*src))
			*dst++ = tolower(*src++);
	*dst = '\0';
}

// TODO: Do some more sanity checks on the input
ssize_t ws_handshake(char* output, size_t output_maxlen, const char* input)
{
	bool ok = false;
	struct http_req req = {};
	if (http_req_parse(&req, input) < 0)
		return -1;

	char protocols[256] = ",";
	char versions[256] = ",";
	char tmpstring[256];

	const char *challenge = NULL;
	for (size_t i = 0; i < req.field_index; ++i) {
		if (strcasecmp(req.field[i].key, "Sec-WebSocket-Key") == 0) {
			challenge = req.field[i].value;
		}
		if (strcasecmp(req.field[i].key, "Sec-WebSocket-Protocol") == 0) {
			snprintf(tmpstring, sizeof(tmpstring), "%s%s,",
					protocols, req.field[i].value);
			tolower_and_remove_ws(protocols, tmpstring);
		}
		if (strcasecmp(req.field[i].key, "Sec-WebSocket-Version") == 0) {
			snprintf(tmpstring, sizeof(tmpstring), "%s%s,",
					versions, req.field[i].value);
			tolower_and_remove_ws(versions, tmpstring);
		}
	}

	if (!challenge)
		goto failure;

	bool have_protocols = strlen(protocols) != 1;
	bool have_versions = strlen(versions) != 1;

	if (have_protocols && !strstr(protocols, ",chat,"))
		goto failure;

	if (have_versions && !strstr(versions, ",13,"))
		goto failure;

	uint8_t hash[20];
	crypto_hash_many(hash, sizeof(hash), CRYPTO_HASH_SHA1,
			(struct crypto_data_entry[]){
		{ (uint8_t*)challenge, strlen(challenge) },
		{ (uint8_t*)magic_uuid, strlen(magic_uuid) },
		{}
	});

	char response[BASE64_ENCODED_SIZE(sizeof(hash))] = {};
	base64_encode(response, hash, sizeof(hash));

	size_t len = snprintf(output, output_maxlen,
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: %s\r\n"
			"%s%s"
			"\r\n",
			response,
			have_protocols ? "Sec-WebSocket-Protocol: char\r\n" : "",
			have_versions ? "Sec-WebSocket-Version: 13\r\n" : "");

	ssize_t header_len = req.header_length;
	ok = len < output_maxlen;
failure:
	http_req_free(&req);
	return ok ? header_len : -1;
}
