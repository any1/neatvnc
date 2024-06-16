#include "crypto.h"
#include "crypto/nettle/common.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <nettle/md5.h>
#include <nettle/sha1.h>
#include <nettle/sha.h>

struct crypto_hash {
	union {
		struct md5_ctx md5;
		struct sha1_ctx sha1;
		struct sha256_ctx sha256;
	} ctx;

	void (*update)(void* ctx, size_t len, const uint8_t* src);
	void (*digest)(void* ctx, size_t len, uint8_t* dst);
};

struct crypto_hash* crypto_hash_new(enum crypto_hash_type type)
{
	struct crypto_hash* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	switch (type) {
	case CRYPTO_HASH_INVALID:
		nvnc_log(NVNC_LOG_PANIC, "Invalid hash type");
		break;
	case CRYPTO_HASH_MD5:
		md5_init(&self->ctx.md5);
		self->update = (void*)nettle_md5_update;
		self->digest = (void*)nettle_md5_digest;
		break;
	case CRYPTO_HASH_SHA1:
		sha1_init(&self->ctx.sha1);
		self->update = (void*)nettle_sha1_update;
		self->digest = (void*)nettle_sha1_digest;
		break;
	case CRYPTO_HASH_SHA256:
		sha256_init(&self->ctx.sha256);
		self->update = (void*)nettle_sha256_update;
		self->digest = (void*)nettle_sha256_digest;
		break;
	}

	return self;
}

void crypto_hash_del(struct crypto_hash* self)
{
	free(self);
}

void crypto_hash_append(struct crypto_hash* self, const uint8_t* src,
		size_t len)
{
	self->update(&self->ctx, len, src);
}

void crypto_hash_digest(struct crypto_hash* self, uint8_t* dst, size_t len)
{
	self->digest(&self->ctx, len, dst);
}

void crypto_hash_one(uint8_t* dst, size_t dst_len, enum crypto_hash_type type,
		const uint8_t* src, size_t src_len)
{
	struct crypto_hash *hash = crypto_hash_new(type);
	crypto_hash_append(hash, src, src_len);
	crypto_hash_digest(hash, dst, dst_len);
	crypto_hash_del(hash);
}

void crypto_hash_many(uint8_t* dst, size_t dst_len, enum crypto_hash_type type,
		const struct crypto_data_entry *src)
{
	struct crypto_hash *hash = crypto_hash_new(type);
	for (int i = 0; src[i].data && src[i].len; ++i)
		crypto_hash_append(hash, src[i].data, src[i].len);
	crypto_hash_digest(hash, dst, dst_len);
	crypto_hash_del(hash);
}

