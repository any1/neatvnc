/*
 * Copyright (c) 2019 - 2024 Andri Yngvason
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

#include "config.h"
#include "auth/auth.h"
#include "neatvnc.h"

#include <string.h>

#ifdef HAVE_CRYPTO
#include "auth/des-auth.h"
#endif

#define EXPORT __attribute__((visibility("default")))

EXPORT
bool nvnc_auth_creds_verify(const struct nvnc_auth_creds* creds,
		const char* password)
{
	if (!password)
		return false;

	switch (creds->type) {
	case NVNC_AUTH_CREDS_PLAIN:
		if (!creds->password)
			return false;
		return strcmp(creds->password, password) == 0;
#ifdef HAVE_CRYPTO
	case NVNC_AUTH_CREDS_DES:
		return des_auth_verify(creds->des.challenge,
				creds->des.response, password);
#endif
	}
	return false;
}

EXPORT
const char* nvnc_auth_creds_get_username(const struct nvnc_auth_creds* creds)
{
	return creds->username;
}

EXPORT
const char* nvnc_auth_creds_get_password(const struct nvnc_auth_creds* creds)
{
	if (creds->type == NVNC_AUTH_CREDS_PLAIN)
		return creds->password;
	return NULL;
}
