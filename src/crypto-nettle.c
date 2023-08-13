#include "crypto.h"
#include "neatvnc.h"

#include <gmp.h>
#include <nettle/base64.h>
#include <nettle/base16.h>
#include <nettle/aes.h>
#include <nettle/md5.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// TODO: This is linux specific
#include <sys/random.h>

struct crypto_key {
	int g;
	mpz_t p;
	mpz_t q;
};

struct crypto_cipher {
	union {
		struct aes128_ctx aes128_ecb;
	} enc_ctx;

	union {
		struct aes128_ctx aes128_ecb;
	} dec_ctx;

	bool (*encrypt)(struct crypto_cipher*, uint8_t* dst, const uint8_t* src,
			size_t len);
	bool (*decrypt)(struct crypto_cipher*, uint8_t* dst, const uint8_t* src,
			size_t len);
};

struct crypto_hash {
	union {
		struct md5_ctx md5;
	} ctx;

	void (*update)(void* ctx, size_t len, const uint8_t* src);
	void (*digest)(void* ctx, size_t len, uint8_t* dst);
};

void crypto_dump_base64(const char* msg, const uint8_t* bytes, size_t len)
{
	struct base64_encode_ctx ctx = {};
	size_t buflen = BASE64_ENCODE_LENGTH(len);
	char* buffer = malloc(buflen + BASE64_ENCODE_FINAL_LENGTH);
	assert(buffer);
	nettle_base64_encode_init(&ctx);
	nettle_base64_encode_update(&ctx, buffer, len, bytes);
	nettle_base64_encode_final(&ctx, buffer + buflen);

	nvnc_log(NVNC_LOG_DEBUG, "%s: %s", msg, buffer);
	free(buffer);
}

struct crypto_key *crypto_key_new(int g, const uint8_t* p, uint32_t p_len,
		const uint8_t* q, uint32_t q_len)
{
	struct crypto_key* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->g = g;

	int order = 1;
	int unit_size = 1;
	int endian = 1;
	int skip_bits = 0;

	mpz_init(self->p);
	mpz_import(self->p, p_len, order, unit_size, endian, skip_bits, p);

	mpz_init(self->q);
	mpz_import(self->q, q_len, order, unit_size, endian, skip_bits, q);

	return self;
}

void crypto_key_del(struct crypto_key* key)
{
	mpz_clear(key->q);
	mpz_clear(key->p);
	free(key);
}

int crypto_key_g(const struct crypto_key* key)
{
	return key->g;
}

uint32_t crypto_key_p(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size)
{
	int order = 1; // msb first
	int unit_size = 1; // byte
	int endian = 1; // msb first
	int skip_bits = 0;

	size_t len = 0;
	mpz_export(dst, &len, order, unit_size, endian, skip_bits, key->p);

	return len;
}

uint32_t crypto_key_q(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size)
{
	int order = 1; // msb first
	int unit_size = 1; // byte
	int endian = 1; // msb first
	int skip_bits = 0;

	size_t len = 0;
	mpz_export(dst, &len, order, unit_size, endian, skip_bits, key->q);

	return len;
}

static void initialise_p(mpz_t p)
{
	// RFC 3526, section 3
	static const char s[] =
		"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
		"29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
		"EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
		"E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
		"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
		"C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
		"83655D23DCA3AD961C62F356208552BB9ED529077096966D"
		"670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
		"E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
		"DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
		"15728E5A8AACAA68FFFFFFFFFFFFFFFF";

	char buf[256];
	size_t len = 0;
	struct base16_decode_ctx ctx;
	nettle_base16_decode_init(&ctx);
	nettle_base16_decode_update(&ctx, &len, (uint8_t*)buf, sizeof(s) - 1, s);
	nettle_base16_decode_final(&ctx);
	assert(len == sizeof(buf));

	int order = 1;
	int unit_size = 1;
	int endian = 1;
	int skip_bits = 0;

	mpz_import(p, sizeof(buf), order, unit_size, endian, skip_bits, buf);
}

static void generate_random(mpz_t n)
{
	uint8_t buf[256];
	getrandom(buf, sizeof(buf), 0);

	int order = 1;
	int unit_size = 1;
	int endian = 1;
	int skip_bits = 0;

	mpz_import(n, sizeof(buf), order, unit_size, endian, skip_bits, buf);
}

struct crypto_key* crypto_keygen(void)
{
	struct crypto_key* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->g = 2;

	mpz_init(self->p);
	initialise_p(self->p);

	mpz_init(self->q);
	generate_random(self->q);

	return self;
}

struct crypto_key* crypto_derive_public_key(const struct crypto_key* priv)
{
	struct crypto_key* pub = calloc(1, sizeof(*pub));
	if (!pub)
		return NULL;

	pub->g = priv->g;
	mpz_set(pub->p, priv->p);
	mpz_init(pub->q);

	mpz_t g;
	mpz_init(g);
	mpz_set_ui(g, priv->g);

	mpz_powm_sec(pub->q, g, priv->q, priv->p);
	mpz_clear(g);

	return pub;
}

struct crypto_key* crypto_derive_shared_secret(
		const struct crypto_key* own_secret,
		const struct crypto_key* remote_public_key)
{
	if (own_secret->g != remote_public_key->g) {
		return NULL;
	}

	if (mpz_cmp(own_secret->p, remote_public_key->p) != 0) {
		return NULL;
	}

	struct crypto_key* shared = calloc(1, sizeof(*shared));
	if (!shared)
		return NULL;

	shared->g = own_secret->g;
	mpz_set(shared->p, own_secret->p);

	mpz_t g;
	mpz_init(g);
	mpz_set_ui(g, own_secret->g);

	mpz_powm_sec(shared->q, remote_public_key->q, own_secret->q,
			own_secret->p);
	mpz_clear(g);

	return shared;
}

static bool crypto_cipher_aes128_ecb_encrypt(struct crypto_cipher* self,
		uint8_t* dst, const uint8_t* src, size_t len)
{
	aes128_encrypt(&self->enc_ctx.aes128_ecb, len, dst, src);
	return true;
}

static bool crypto_cipher_aes128_ecb_decrypt(struct crypto_cipher* self,
		uint8_t* dst, const uint8_t* src, size_t len)
{
	aes128_decrypt(&self->dec_ctx.aes128_ecb, len, dst, src);
	return true;
}

static struct crypto_cipher* crypto_cipher_new_aes128_ecb(const uint8_t* key)
{
	struct crypto_cipher* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	aes128_set_encrypt_key(&self->enc_ctx.aes128_ecb, key);
	aes128_invert_key(&self->dec_ctx.aes128_ecb, &self->enc_ctx.aes128_ecb);

	self->encrypt = crypto_cipher_aes128_ecb_encrypt;
	self->decrypt = crypto_cipher_aes128_ecb_decrypt;

	return self;
}

struct crypto_cipher* crypto_cipher_new(const uint8_t* key,
		enum crypto_cipher_type type)
{
	switch (type) {
	case CRYPTO_CIPHER_AES128_ECB:
		return crypto_cipher_new_aes128_ecb(key);
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

bool crypto_cipher_encrypt(struct crypto_cipher* self, uint8_t* dst,
		const uint8_t* src, size_t len)
{
	return self->encrypt(self, dst, src, len);
}

bool crypto_cipher_decrypt(struct crypto_cipher* self, uint8_t* dst,
		const uint8_t* src, size_t len)
{
	return self->decrypt(self, dst, src, len);
}

struct crypto_hash* crypto_hash_new(enum crypto_hash_type type)
{
	struct crypto_hash* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	md5_init(&self->ctx.md5);
	self->update = (void*)nettle_md5_update;
	self->digest = (void*)nettle_md5_digest;

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
