#include "crypto.h"

#include <stdint.h>

// TODO: This is linux specific
#include <sys/random.h>

void crypto_random(uint8_t* dst, size_t len)
{
	getrandom(dst, len, 0);
}
