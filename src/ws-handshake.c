#include "websocket.h"
#include "http.h"

#include <nettle/sha1.h>
#include <nettle/base64.h>

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

	struct sha1_ctx ctx;
	sha1_init(&ctx);
	sha1_update(&ctx, strlen(challenge), (const uint8_t*)challenge);
	sha1_update(&ctx, strlen(magic_uuid), (const uint8_t*)magic_uuid);

	uint8_t hash[SHA1_DIGEST_SIZE];
	sha1_digest(&ctx, sizeof(hash), hash);

	char response[BASE64_ENCODE_RAW_LENGTH(SHA1_DIGEST_SIZE) + 1] = {};
	base64_encode_raw(response, SHA1_DIGEST_SIZE, hash);

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
