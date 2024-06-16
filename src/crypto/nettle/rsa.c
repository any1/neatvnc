#include "crypto.h"
#include "crypto/nettle/common.h"
#include "vec.h"
#include "neatvnc.h"
#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <nettle/rsa.h>

struct crypto_rsa_pub_key {
	struct rsa_public_key key;
};

struct crypto_rsa_priv_key {
	struct rsa_private_key key;
};

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
	crypto_random(dst, len);
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
