#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

struct crypto_key;
struct crypto_cipher;
struct crypto_hash;

enum crypto_cipher_type {
	CRYPTO_CIPHER_INVALID = 0,
	CRYPTO_CIPHER_AES128_ECB,
};

enum crypto_hash_type {
	CRYPTO_HASH_INVALID = 0,
	CRYPTO_HASH_MD5,
};

void crypto_dump_base64(const char* msg, const uint8_t* bytes, size_t len);

void crypto_cleanup(void);

// Key generation
struct crypto_key* crypto_key_new(int g, const uint8_t *p, uint32_t p_len,
		const uint8_t* q, uint32_t q_len);
void crypto_key_del(struct crypto_key* key);

int crypto_key_g(const struct crypto_key* key);
uint32_t crypto_key_p(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size);
uint32_t crypto_key_q(const struct crypto_key* key, uint8_t* dst,
		uint32_t dst_size);

struct crypto_key* crypto_keygen(void);

// Diffie-Hellman
struct crypto_key* crypto_derive_public_key(const struct crypto_key* priv);
struct crypto_key* crypto_derive_shared_secret(
		const struct crypto_key* own_secret,
		const struct crypto_key* remote_public_key);

// Ciphers
struct crypto_cipher* crypto_cipher_new(const uint8_t* key,
		enum crypto_cipher_type type);
void crypto_cipher_del(struct crypto_cipher* self);

bool crypto_cipher_encrypt(struct crypto_cipher* self, uint8_t* dst,
		const uint8_t* src, size_t len);
bool crypto_cipher_decrypt(struct crypto_cipher* self, uint8_t* dst,
		const uint8_t* src, size_t len);

// Hashing
struct crypto_hash* crypto_hash_new(enum crypto_hash_type type);
void crypto_hash_del(struct crypto_hash* self);

void crypto_hash_append(struct crypto_hash* self, const uint8_t* src,
		size_t len);
void crypto_hash_digest(struct crypto_hash* self, uint8_t* dst,
		size_t len);
