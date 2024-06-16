#include "crypto.h"
#include "crypto/nettle/common.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <gmp.h>
#include <nettle/base16.h>
#include <nettle/base64.h>

struct crypto_key {
	int g;
	mpz_t p;
	mpz_t q;
};

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
	crypto_random(buf, sizeof(buf));
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

