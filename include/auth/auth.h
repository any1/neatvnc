/*
 * Copyright (c) 2023 - 2024 Andri Yngvason
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

#include "weakref.h"
#include <stdint.h>

#define NVNC_AUTH_DES_CHALLENGE_SIZE 16
#define NVNC_AUTH_USERNAME_MAX 256
#define NVNC_AUTH_PASSWORD_MAX 256

struct nvnc_client;

enum nvnc_auth_creds_type {
	NVNC_AUTH_CREDS_PLAIN,
	NVNC_AUTH_CREDS_DES,
};

struct nvnc_auth_creds {
	enum nvnc_auth_creds_type type;
	char username[NVNC_AUTH_USERNAME_MAX];
	union {
		char password[NVNC_AUTH_PASSWORD_MAX];
		struct {
			uint8_t challenge[NVNC_AUTH_DES_CHALLENGE_SIZE];
			uint8_t response[NVNC_AUTH_DES_CHALLENGE_SIZE];
		} des;
	};
};

struct nvnc_auth_future {
	struct weakref_observer client;
	struct nvnc_auth_creds creds;
};

int security_handshake_ok(struct nvnc_client* client);
int security_handshake_failed(struct nvnc_client* client, const char* reason);

void security_handshake_authenticate(struct nvnc_client* client,
		const struct nvnc_auth_creds* creds);
