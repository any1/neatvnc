#include "crypto.h"
#include "crypto/nettle/common.h"
#include "vec.h"
#include "neatvnc.h"

#include <stdlib.h>
#include <nettle/aes.h>
#include <nettle/eax.h>

struct crypto_aes_eax {
	struct eax_aes128_ctx ctx;
	uint64_t count[2];
};

struct crypto_aes256_eax {
	struct EAX_CTX(struct aes256_ctx) ctx;
	uint64_t count[2];
};

struct crypto_cipher {
	union {
		struct aes128_ctx aes128_ecb;
		struct crypto_aes_eax aes_eax;
		struct crypto_aes256_eax aes256_eax;
	} enc_ctx;

	union {
		struct aes128_ctx aes128_ecb;
		struct crypto_aes_eax aes_eax;
		struct crypto_aes256_eax aes256_eax;
	} dec_ctx;

	bool (*encrypt)(struct crypto_cipher*, struct vec* dst, uint8_t* mac,
			const uint8_t* src, size_t src_len, const uint8_t* ad,
			size_t ad_len);
	ssize_t (*decrypt)(struct crypto_cipher*, uint8_t* dst, uint8_t* mac,
			const uint8_t* src, size_t src_len, const uint8_t* ad,
			size_t ad_len);
};

static bool crypto_cipher_aes128_ecb_encrypt(struct crypto_cipher* self,
		struct vec* dst, uint8_t* mac, const uint8_t* src,
		size_t len, const uint8_t* ad, size_t ad_len)
{
	vec_reserve(dst, dst->len + len);
	aes128_encrypt(&self->enc_ctx.aes128_ecb, len, dst->data, src);
	dst->len = len;
	return true;
}

static ssize_t crypto_cipher_aes128_ecb_decrypt(struct crypto_cipher* self,
		uint8_t* dst, uint8_t* mac, const uint8_t* src, size_t len,
		const uint8_t* ad, size_t ad_len)
{
	aes128_decrypt(&self->dec_ctx.aes128_ecb, len, dst, src);
	return len;
}

static struct crypto_cipher* crypto_cipher_new_aes128_ecb(
		const uint8_t* enc_key, const uint8_t* dec_key)
{
	struct crypto_cipher* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	if (enc_key)
		aes128_set_encrypt_key(&self->enc_ctx.aes128_ecb, enc_key);

	if (dec_key)
		aes128_set_decrypt_key(&self->dec_ctx.aes128_ecb, dec_key);

	self->encrypt = crypto_cipher_aes128_ecb_encrypt;
	self->decrypt = crypto_cipher_aes128_ecb_decrypt;

	return self;
}

static void crypto_aes_eax_update_nonce(struct crypto_aes_eax* self)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	nettle_eax_aes128_set_nonce(&self->ctx, 16, (const uint8_t*)self->count);
#else
	uint64_t c[2];
	c[0] = __builtin_bswap64(self->count[0]);
	c[1] = __builtin_bswap64(self->count[1]);
	nettle_eax_aes128_set_nonce(&self->ctx, 16, (const uint8_t*)c);
#endif

	if (++self->count[0] == 0)
		++self->count[1];
}

static bool crypto_cipher_aes_eax_encrypt(struct crypto_cipher* self,
		struct vec* dst, uint8_t* mac, const uint8_t* src,
		size_t src_len, const uint8_t* ad, size_t ad_len)
{
	vec_reserve(dst, dst->len + src_len);

	crypto_aes_eax_update_nonce(&self->enc_ctx.aes_eax);
	nettle_eax_aes128_update(&self->enc_ctx.aes_eax.ctx, ad_len,
			(uint8_t*)ad);
	nettle_eax_aes128_encrypt(&self->enc_ctx.aes_eax.ctx, src_len,
			(uint8_t*)dst->data + dst->len, src);
	dst->len += src_len;

	nettle_eax_aes128_digest(&self->enc_ctx.aes_eax.ctx, 16, mac);

	return true;
}

static ssize_t crypto_cipher_aes_eax_decrypt(struct crypto_cipher* self,
		uint8_t* dst, uint8_t* mac, const uint8_t* src, size_t len,
		const uint8_t* ad, size_t ad_len)
{
	crypto_aes_eax_update_nonce(&self->dec_ctx.aes_eax);
	nettle_eax_aes128_update(&self->dec_ctx.aes_eax.ctx, ad_len, ad);
	nettle_eax_aes128_decrypt(&self->dec_ctx.aes_eax.ctx, len, dst, src);
	nettle_eax_aes128_digest(&self->dec_ctx.aes_eax.ctx, 16, mac);
	return len;
}

static struct crypto_cipher* crypto_cipher_new_aes_eax(const uint8_t* enc_key,
		const uint8_t* dec_key)
{
	struct crypto_cipher* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	eax_aes128_set_key(&self->enc_ctx.aes_eax.ctx, enc_key);
	eax_aes128_set_key(&self->dec_ctx.aes_eax.ctx, dec_key);

	self->encrypt = crypto_cipher_aes_eax_encrypt;
	self->decrypt = crypto_cipher_aes_eax_decrypt;

	return self;
}

static void crypto_aes256_eax_update_nonce(struct crypto_aes256_eax* self)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	EAX_SET_NONCE(&self->ctx, aes256_encrypt, 16, (const uint8_t*)self->count);
#else
	uint64_t c[2];
	c[0] = __builtin_bswap64(self->count[0]);
	c[1] = __builtin_bswap64(self->count[1]);
	EAX_SET_NONCE(&self->ctx, aes256_encrypt, 16, (const uint8_t*)c);
#endif

	if (++self->count[0] == 0)
		++self->count[1];
}

static bool crypto_cipher_aes256_eax_encrypt(struct crypto_cipher* self,
		struct vec* dst, uint8_t* mac, const uint8_t* src,
		size_t src_len, const uint8_t* ad, size_t ad_len)
{
	vec_reserve(dst, dst->len + src_len);

	crypto_aes256_eax_update_nonce(&self->enc_ctx.aes256_eax);

	EAX_UPDATE(&self->enc_ctx.aes256_eax.ctx, aes256_encrypt, ad_len, ad);
	EAX_ENCRYPT(&self->enc_ctx.aes256_eax.ctx, aes256_encrypt, src_len,
			(uint8_t*)dst->data + dst->len, src);
	dst->len += src_len;

	EAX_DIGEST(&self->enc_ctx.aes256_eax.ctx, aes256_encrypt, 16, mac);

	return true;
}

static ssize_t crypto_cipher_aes256_eax_decrypt(struct crypto_cipher* self,
		uint8_t* dst, uint8_t* mac, const uint8_t* src, size_t len,
		const uint8_t* ad, size_t ad_len)
{
	crypto_aes256_eax_update_nonce(&self->dec_ctx.aes256_eax);
	EAX_UPDATE(&self->dec_ctx.aes256_eax.ctx, aes256_encrypt, ad_len, ad);
	EAX_DECRYPT(&self->dec_ctx.aes256_eax.ctx, aes256_encrypt, len, dst, src);
	EAX_DIGEST(&self->dec_ctx.aes256_eax.ctx, aes256_encrypt, 16, mac);
	return len;
}

static struct crypto_cipher* crypto_cipher_new_aes256_eax(const uint8_t* enc_key,
		const uint8_t* dec_key)
{
	struct crypto_cipher* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	EAX_SET_KEY(&self->enc_ctx.aes256_eax.ctx, aes256_set_encrypt_key,
			aes256_encrypt, enc_key);
	EAX_SET_KEY(&self->dec_ctx.aes256_eax.ctx, aes256_set_encrypt_key,
			aes256_encrypt, dec_key);

	self->encrypt = crypto_cipher_aes256_eax_encrypt;
	self->decrypt = crypto_cipher_aes256_eax_decrypt;

	return self;
}

struct crypto_cipher* crypto_cipher_new(const uint8_t* enc_key,
		const uint8_t* dec_key, enum crypto_cipher_type type)
{
	switch (type) {
	case CRYPTO_CIPHER_AES128_ECB:
		return crypto_cipher_new_aes128_ecb(enc_key, dec_key);
	case CRYPTO_CIPHER_AES_EAX:
		return crypto_cipher_new_aes_eax(enc_key, dec_key);
	case CRYPTO_CIPHER_AES256_EAX:
		return crypto_cipher_new_aes256_eax(enc_key, dec_key);
	case CRYPTO_CIPHER_INVALID:
		break;
	}

	nvnc_log(NVNC_LOG_PANIC, "Invalid type: %d", type);
	return NULL;
}

void crypto_cipher_del(struct crypto_cipher* self)
{
	free(self);
}

bool crypto_cipher_encrypt(struct crypto_cipher* self, struct vec* dst,
		uint8_t* mac, const uint8_t* src, size_t src_len,
		const uint8_t* ad, size_t ad_len)
{
	return self->encrypt(self, dst, mac, src, src_len, ad, ad_len);
}

ssize_t crypto_cipher_decrypt(struct crypto_cipher* self, uint8_t* dst,
		uint8_t* mac, const uint8_t* src, size_t src_len,
		const uint8_t* ad, size_t ad_len)
{
	return self->decrypt(self, dst, mac, src, src_len, ad, ad_len);
}
