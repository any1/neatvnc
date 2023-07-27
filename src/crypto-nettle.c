#include "crypto.h"
#include "neatvnc.h"
#include "vec.h"
#include "base64.h"

#include <gmp.h>
#include <nettle/base64.h>
#include <nettle/base16.h>
#include <nettle/aes.h>
#include <nettle/eax.h>
#include <nettle/md5.h>
#include <nettle/sha1.h>
#include <nettle/sha.h>
#include <nettle/rsa.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>
#include <arpa/inet.h>

// TODO: This is linux specific
#include <sys/random.h>

#define UDIV_UP(a, b) (((a) + (b) - 1) / (b))

struct vec;

struct crypto_key {
	int g;
	mpz_t p;
	mpz_t q;
};

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

struct crypto_hash {
	union {
		struct md5_ctx md5;
		struct sha1_ctx sha1;
		struct sha256_ctx sha256;
	} ctx;

	void (*update)(void* ctx, size_t len, const uint8_t* src);
	void (*digest)(void* ctx, size_t len, uint8_t* dst);
};

struct crypto_rsa_pub_key {
	struct rsa_public_key key;
};

struct crypto_rsa_priv_key {
	struct rsa_private_key key;
};

void crypto_dump_base64(const char* msg, const uint8_t* bytes, size_t len)
{
	struct base64_encode_ctx ctx = {};
	size_t buflen = BASE64_ENCODE_LENGTH(len);
	char* buffer = malloc(buflen + BASE64_ENCODE_FINAL_LENGTH + 1);
	assert(buffer);
	nettle_base64_encode_init(&ctx);
	size_t count = nettle_base64_encode_update(&ctx, buffer, len, bytes);
	count += nettle_base64_encode_final(&ctx, buffer + count);
	buffer[count] = '\0';

	nvnc_log(NVNC_LOG_DEBUG, "%s: %s", msg, buffer);
	free(buffer);
}

void crypto_dump_base16(const char* msg, const uint8_t* bytes, size_t len)
{
	size_t buflen = BASE16_ENCODE_LENGTH(len);
	char* buffer = calloc(1, buflen + 1);
	assert(buffer);
	nettle_base16_encode_update(buffer, len, bytes);

	nvnc_log(NVNC_LOG_DEBUG, "%s: %s", msg, buffer);
	free(buffer);
}

void crypto_random(uint8_t* dst, size_t len)
{
	getrandom(dst, len, 0);
}

static void crypto_import(mpz_t n, const uint8_t* src, size_t len)
{
	int order = 1;
	int unit_size = 1;
	int endian = 1;
	int skip_bits = 0;

	mpz_import(n, len, order, unit_size, endian, skip_bits, src);
}

struct crypto_key *crypto_key_new(int g, const uint8_t* p, uint32_t p_len,
		const uint8_t* q, uint32_t q_len)
{
	struct crypto_key* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->g = g;

	mpz_init(self->p);
	crypto_import(self->p, p, p_len);

	mpz_init(self->q);
	crypto_import(self->q, q, q_len);

	return self;
}

void crypto_key_del(struct crypto_key* key)
{
	if (!key)
		return;
	mpz_clear(key->q);
	mpz_clear(key->p);
	free(key);
}

int crypto_key_g(const struct crypto_key* key)
{
	return key->g;
}

static size_t crypto_export(uint8_t* dst, size_t dst_size, const mpz_t n)
{
	int order = 1; // msb first
	int unit_size = 1; // byte
	int endian = 1; // msb first
	int skip_bits = 0;

	size_t bitsize = mpz_sizeinbase(n, 2);
	size_t bytesize = (bitsize + 7) / 8;

	assert(bytesize <= dst_size);

	memset(dst, 0, dst_size);
	mpz_export(dst + dst_size - bytesize, &bytesize, order, unit_size,
			endian, skip_bits, n);

	return bytesize;
}

uint32_t crypto_key_p(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size)
{
	return crypto_export(dst, dst_size, key->p);
}

uint32_t crypto_key_q(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size)
{
	return crypto_export(dst, dst_size, key->q);
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

	crypto_import(p, (const uint8_t*)buf, sizeof(buf));
}

static void generate_random(mpz_t n)
{
	uint8_t buf[256];
	getrandom(buf, sizeof(buf), 0);
	crypto_import(n, buf, sizeof(buf));
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
		aes128_set_decrypt_key(&self->enc_ctx.aes128_ecb, dec_key);

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

struct crypto_rsa_pub_key *crypto_rsa_pub_key_new(void)
{
	struct crypto_rsa_pub_key* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	rsa_public_key_init(&self->key);
	return self;
}

void crypto_rsa_pub_key_del(struct crypto_rsa_pub_key* self)
{
	if (!self)
		return;
	rsa_public_key_clear(&self->key);
	free(self);
}

struct crypto_rsa_pub_key* crypto_rsa_pub_key_import(const uint8_t* modulus,
		const uint8_t* exponent, size_t size)
{
	struct crypto_rsa_pub_key* self = crypto_rsa_pub_key_new();
	if (!self)
		return NULL;

	rsa_public_key_init(&self->key);
	mpz_init(self->key.n);
	crypto_import(self->key.n, modulus, size);
	mpz_init(self->key.e);
	crypto_import(self->key.e, exponent, size);
	rsa_public_key_prepare(&self->key);

	return self;
}

bool crypto_rsa_priv_key_import_pkcs1_der(struct crypto_rsa_priv_key* priv,
		struct crypto_rsa_pub_key* pub, const uint8_t* key,
		size_t size)
{
	return rsa_keypair_from_der(&pub->key, &priv->key, 0, size, key);
}

bool crypto_rsa_priv_key_load(struct crypto_rsa_priv_key* priv,
		struct crypto_rsa_pub_key* pub, const char* path)
{
	FILE* stream = fopen(path, "r");
	if (!stream) {
		nvnc_log(NVNC_LOG_ERROR, "Could not open file: %m");
		return false;
	}

	char* line = NULL;
	size_t n = 0;
	if (getline(&line, &n, stream) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "RSA private key file is not PEM");
		return false;
	}

	char head[128];
	strncpy(head, line, sizeof(head));
	head[sizeof(head) - 1] = '\0';
	char* end = strchr(head, '\n');
	if (end)
		*end = '\0';

	nvnc_trace("Read PEM head: \"%s\"\n", head);

	struct vec base64_der;
	vec_init(&base64_der, 4096);

	while (getline(&line, &n, stream) >= 0) {
		if (strncmp(line, "-----END", 8) == 0)
			break;


		vec_append(&base64_der, line, strcspn(line, "\n"));
	}
	free(line);
	fclose(stream);

	vec_append_zero(&base64_der, 1);

	uint8_t* der = malloc(BASE64_DECODED_MAX_SIZE(base64_der.len));
	assert(der);

	ssize_t der_len = base64_decode(der, base64_der.data);
	vec_destroy(&base64_der);
	if (der_len < 0) {
		free(der);
		return false;
	}

	bool ok = false;
	if (strcmp(head, "-----BEGIN RSA PRIVATE KEY-----") == 0) {
		ok = crypto_rsa_priv_key_import_pkcs1_der(priv, pub, der, der_len);
	} else {
		nvnc_log(NVNC_LOG_ERROR, "Unsupported RSA private key format");
	}

	nvnc_trace("Private key is %d bits long", priv->key.size * 8);

	free(der);
	return ok;
}

void crypto_rsa_pub_key_modulus(const struct crypto_rsa_pub_key* key,
		uint8_t* dst, size_t dst_size)
{
	crypto_export(dst, dst_size, key->key.n);
}

void crypto_rsa_pub_key_exponent(const struct crypto_rsa_pub_key* key,
		uint8_t* dst, size_t dst_size)
{
	char* str = mpz_get_str(NULL, 16, key->key.e);
	free(str);

	crypto_export(dst, dst_size, key->key.e);
}

struct crypto_rsa_priv_key *crypto_rsa_priv_key_new(void)
{
	struct crypto_rsa_priv_key* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	rsa_private_key_init(&self->key);
	return self;
}

void crypto_rsa_priv_key_del(struct crypto_rsa_priv_key* self)
{
	if (!self)
		return;
	rsa_private_key_clear(&self->key);
	free(self);
}

size_t crypto_rsa_pub_key_length(const struct crypto_rsa_pub_key* key)
{
	return key->key.size;
}

static void generate_random_for_rsa(void* random_ctx, size_t len, uint8_t* dst)
{
	getrandom(dst, len, 0);
}

bool crypto_rsa_keygen(struct crypto_rsa_pub_key* pub,
		struct crypto_rsa_priv_key* priv)
{
	void* random_ctx = NULL;
	nettle_random_func* random_func = generate_random_for_rsa;
	void* progress_ctx = NULL;
	nettle_progress_func* progress = NULL;

	int rc = rsa_generate_keypair(&pub->key, &priv->key, random_ctx,
			random_func, progress_ctx, progress, 2048, 30);
	return rc != 0;
}

ssize_t crypto_rsa_encrypt(struct crypto_rsa_pub_key* pub, uint8_t* dst,
		size_t dst_size, const uint8_t* src, size_t src_size)
{
	mpz_t ciphertext;
	mpz_init(ciphertext);
	int r = rsa_encrypt(&pub->key, NULL, generate_random_for_rsa,
			src_size, src, ciphertext);
	if (r == 0) {
		mpz_clear(ciphertext);
		return -1;
	}
	size_t len = crypto_export(dst, dst_size, ciphertext);
	mpz_clear(ciphertext);
	return len;
}

ssize_t crypto_rsa_decrypt(struct crypto_rsa_priv_key* priv, uint8_t* dst,
		size_t dst_size, const uint8_t* src, size_t src_size)
{
	mpz_t ciphertext;
	mpz_init(ciphertext);
	crypto_import(ciphertext, src, src_size);
	int r = rsa_decrypt(&priv->key, &dst_size, dst, ciphertext);
	mpz_clear(ciphertext);
	return r != 0 ? (ssize_t)dst_size : -1;
}
