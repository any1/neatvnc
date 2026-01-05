#pragma once

#include <stdint.h>

struct nvnc_client;

void vnc_auth_reverse_bits(uint8_t *dst, uint8_t *src);
int vnc_auth_send_challenge(struct nvnc_client* client);
int vnc_auth_handle_response(struct nvnc_client* client);
