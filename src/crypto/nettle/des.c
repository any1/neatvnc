#include "crypto.h"

#include <stdint.h>
#include <nettle/des.h>

void crypto_des_encrypt(uint8_t* key, uint8_t* dst, uint8_t* src, size_t len)
{
	struct des_ctx ctx = {0};
	des_set_key(&ctx, key);
	des_encrypt(&ctx, len, dst, src);
}
