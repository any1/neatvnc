#pragma once

#define CHALLENGESIZE 16
#define KEYSIZE 8

struct nvnc_client;

int vnc_auth_handle_message(struct nvnc_client* client);
void genRandomBytes(unsigned char *bytes);
