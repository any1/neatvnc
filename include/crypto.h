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

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

struct crypto_key;
struct crypto_cipher;
struct crypto_hash;
struct crypto_rsa_pub_key;
struct crypto_rsa_priv_key;
struct vec;

enum crypto_cipher_type {
	CRYPTO_CIPHER_INVALID = 0,
	CRYPTO_CIPHER_AES128_ECB,
	CRYPTO_CIPHER_AES_EAX,
	CRYPTO_CIPHER_AES256_EAX,
};

enum crypto_hash_type {
	CRYPTO_HASH_INVALID = 0,
	CRYPTO_HASH_MD5,
	CRYPTO_HASH_SHA1,
	CRYPTO_HASH_SHA256,
};

struct crypto_data_entry {
	uint8_t* data;
	size_t len;
};

void crypto_dump_base16(const char* msg, const uint8_t* bytes, size_t len);
void crypto_dump_base64(const char* msg, const uint8_t* bytes, size_t len);

void crypto_random(uint8_t* dst, size_t len);

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
struct crypto_cipher* crypto_cipher_new(const uint8_t* enc_key,
		const uint8_t* dec_key, enum crypto_cipher_type type);
void crypto_cipher_del(struct crypto_cipher* self);

bool crypto_cipher_encrypt(struct crypto_cipher* self, struct vec* dst,
		uint8_t* mac, const uint8_t* src, size_t len,
		const uint8_t* ad, size_t ad_len);
ssize_t crypto_cipher_decrypt(struct crypto_cipher* self, uint8_t* dst,
		uint8_t* mac, const uint8_t* src, size_t len,
		const uint8_t* ad, size_t ad_len);

// Hashing
struct crypto_hash* crypto_hash_new(enum crypto_hash_type type);
void crypto_hash_del(struct crypto_hash* self);

void crypto_hash_append(struct crypto_hash* self, const uint8_t* src,
		size_t len);
void crypto_hash_digest(struct crypto_hash* self, uint8_t* dst,
		size_t len);

void crypto_hash_one(uint8_t* dst, size_t dst_len, enum crypto_hash_type type,
		const uint8_t* src, size_t src_len);
void crypto_hash_many(uint8_t* dst, size_t dst_len, enum crypto_hash_type type,
		const struct crypto_data_entry *src);

// RSA
struct crypto_rsa_pub_key* crypto_rsa_pub_key_new(void);
void crypto_rsa_pub_key_del(struct crypto_rsa_pub_key*);

// Returns length in bytes
size_t crypto_rsa_pub_key_length(const struct crypto_rsa_pub_key* key);

struct crypto_rsa_pub_key* crypto_rsa_pub_key_import(const uint8_t* modulus,
		const uint8_t* exponent, size_t size);

void crypto_rsa_pub_key_modulus(const struct crypto_rsa_pub_key* key,
		uint8_t* dst, size_t dst_size);
void crypto_rsa_pub_key_exponent(const struct crypto_rsa_pub_key* key,
		uint8_t* dst, size_t dst_size);

bool crypto_rsa_priv_key_import_pkcs1_der(struct crypto_rsa_priv_key* priv,
		struct crypto_rsa_pub_key* pub, const uint8_t* key,
		size_t size);

bool crypto_rsa_priv_key_load(struct crypto_rsa_priv_key* priv,
		struct crypto_rsa_pub_key* pub, const char* path);

struct crypto_rsa_priv_key *crypto_rsa_priv_key_new(void);
void crypto_rsa_priv_key_del(struct crypto_rsa_priv_key*);

bool crypto_rsa_keygen(struct crypto_rsa_pub_key*, struct crypto_rsa_priv_key*);

ssize_t crypto_rsa_encrypt(struct crypto_rsa_pub_key* pub, uint8_t* dst,
		size_t dst_size, const uint8_t* src, size_t src_size);
ssize_t crypto_rsa_decrypt(struct crypto_rsa_priv_key* priv, uint8_t* dst,
		size_t dst_size, const uint8_t* src, size_t src_size);

// DES
void crypto_des_encrypt(uint8_t* key, uint8_t* dst, uint8_t* src, size_t len);
