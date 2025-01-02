#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include "common.h"
#include "stream/stream.h"
#include "auth/auth.h"
#include "auth/vncauth.h"
#include <openssl/evp.h>


static unsigned char reverseByte(unsigned char b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

int encrypt_des(void *encrypted, int *encrypted_len, const unsigned char deskey[8], const void *plain, const size_t plain_len)
{
    int res = 0;
    EVP_CIPHER_CTX *ctx;
    unsigned char reversedKey[8];

    for (int i = 0; i < 8; i++)
    {
        reversedKey[i] = reverseByte(deskey[i]);
    }
    if(!(ctx = EVP_CIPHER_CTX_new()))
    {
        nvnc_log(NVNC_LOG_ERROR, "EVP_CIPHER_CTX_new() failed");
        goto onFailure;
    }

    if(!EVP_EncryptInit_ex(ctx, EVP_des_ecb(), NULL, reversedKey, NULL))
    {
        nvnc_log(NVNC_LOG_ERROR, "EVP_EncryptInit_ex() failed");
        goto onFailure;
    }
    if(!EVP_EncryptUpdate(ctx, encrypted, encrypted_len, plain, plain_len))
    {
        nvnc_log(NVNC_LOG_ERROR, "EVP_EncryptUpdate()  failed");
        goto onFailure;
    }
    res = 1;

 onFailure:
    EVP_CIPHER_CTX_free(ctx);
    return res;
}

void genRandomBytes(unsigned char *bytes)
{
    if (!bytes)
        return;

    static bool s_srandom_called = false;

    if (!s_srandom_called)
    {
        srandom((unsigned int)time(NULL) ^ (unsigned int)getpid());
        s_srandom_called = true;
    }

    int i = 0;
    for (i = 0; i < CHALLENGESIZE; i++)
    {
        bytes[i] = (unsigned char)(random() & 255);
        nvnc_log(NVNC_LOG_DEBUG, "genRandomBytes: %u", bytes[i]);
    }
}

void encryptBytes(uint8_t *bytes, char *passwd)
{
    if (!bytes)
        return;
    if (!passwd)
        return;
    unsigned char key[8];
    unsigned int i;
    int out_len;

    /* key is simply password padded with nulls */
    for (i = 0; i < 8; i++)
    {
        if (i < strlen(passwd))
        {
            key[i] = passwd[i];
        }
        else
        {
            key[i] = 0;
        }
    }

    uint8_t newin[16];
    unsigned char encryptedPasswd[8];
    memcpy(newin, bytes, 16);

    uint8_t outBuf[CHALLENGESIZE];
    if(encrypt_des(bytes,&out_len, key, newin, CHALLENGESIZE ) != 1)
    {
        nvnc_log(NVNC_LOG_DEBUG, "encryptBytes failed");
    }
}

bool compare_challenges(struct nvnc_client *cl, const char *response, int len)
{
    nvnc_log(NVNC_LOG_DEBUG, "compare_challenges called");

    if (!cl)
        return false;
    if (!response)
        return false;

    char **passwds;
    int i = 0;
    uint8_t auth_tmp[CHALLENGESIZE];
    memcpy((char *)auth_tmp, (char *)cl->vnc_auth.challenge, CHALLENGESIZE);

    for (int i = 0; i < CHALLENGESIZE; i++)
    {
        nvnc_log(NVNC_LOG_DEBUG, "0x%02X ", auth_tmp[i]);
    }
    nvnc_log(NVNC_LOG_DEBUG, " password is: %s\n", cl->auth->password);

    encryptBytes(auth_tmp, cl->auth->password);

    if (memcmp(auth_tmp, response, len) == 0)
    {
        nvnc_log(NVNC_LOG_DEBUG, "Password comparision passed");
        return true;
    }

    nvnc_log(NVNC_LOG_ERROR, "Password comparision failed");
    return false;
}

static int on_vnc_auth_des_challenge_message(struct nvnc_client *client)
{
    if (!client)
    {
        nvnc_log(NVNC_LOG_ERROR, "Client is NULL");
        return 0;
    }

    if (!client->msg_buffer)
    {
        nvnc_log(NVNC_LOG_ERROR, "Message buffer is NULL");
        return 0;
    }

    if (client->buffer_len < client->buffer_index + CHALLENGESIZE)
    {
        nvnc_log(NVNC_LOG_WARNING, "Buffer index out of bounds");
        return 0;
    }

    nvnc_log(NVNC_LOG_DEBUG, "on_vnc_auth_des_challenge_message called");

    uint8_t response[CHALLENGESIZE];
    memcpy(response, client->msg_buffer + client->buffer_index, CHALLENGESIZE);
    response[CHALLENGESIZE] = '\0';

    if (!compare_challenges(client, (const char *)response, CHALLENGESIZE))
    {
        security_handshake_failed(client, "", "Invalid password");
    }
    else
    {
        security_handshake_ok(client, "");
        client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
    }

    return CHALLENGESIZE;
}

int vnc_auth_handle_message(struct nvnc_client *client)
{
    nvnc_log(NVNC_LOG_DEBUG, "vnc_auth_handle_message called");
    if (!client)
        return 0;

    switch (client->state)
    {
    case VNC_CLIENT_STATE_WAITING_FOR_CHALLENGE:
        return on_vnc_auth_des_challenge_message(client);
    default:;
    }

    nvnc_log(NVNC_LOG_DEBUG, "Unhandled client state: %d", client->state);
    return 0;
}
