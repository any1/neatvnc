#ifndef _RFB_PROTO_H
#define _RFB_PROTO_H

#define RFB_VERSION_MESSAGE "RFB 003.008\n"

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#define RFB_PACKED __attribute__((packed))

int send_to_client(void *client, const void *payload, size_t size);
int receive_from_client(void *client, void *result, size_t size);

enum rfb_security_type : uint8_t {
        RFB_SECURITY_TYPE_INVALID = 0,
        RFB_SECURITY_TYPE_NONE = 1,
        RFB_SECURITY_TYPE_VNC_AUTH = 2,
};

enum rfb_security_handshake_result : uint32_t {
	RFB_SECURITY_HANDSHAKE_OK = 0,
	RFB_SECURITY_HANDSHAKE_FAILED = 1,
};

enum rfb_client_to_server_msg_type : uint8_t {
	RFB_CLIENT_TO_SERVER_SET_PIXEL_FORMAT = 0,
	RFB_CLIENT_TO_SERVER_SET_ENCODINGS = 2,
	RFB_CLIENT_TO_SERVER_FRAMEBUFFER_UPDATE_REQUEST = 3,
	RFB_CLIENT_TO_SERVER_KEY_EVENT = 4,
	RFB_CLIENT_TO_SERVER_POINTER_EVENT = 5,
	RFB_CLIENT_TO_SERVER_CLIENT_CUT_TEXT = 6,
};

struct rfb_security_types_msg {
        uint8_t n;
        enum rfb_security_type types[1];
} RFB_PACKED;

struct rfb_error_reason {
	uint32_t length;
	char message[0];
} RFB_PACKED;

struct rfb_pixel_format {
	uint8_t bits_per_pixel;
	uint8_t depth;
	uint8_t big_endian_flag;
	uint8_t true_colour_flag;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
	uint8_t padding[3];
} RFB_PACKED;

struct rfb_server_init_msg {
	uint16_t width;
	uint16_t height;
	struct rfb_pixel_format pixel_format;
	uint32_t name_length;
	char name_string[0];
} RFB_PACKED;

static inline int rfb_send_security_types(void *client)
{
        struct rfb_security_types_msg payload = {
                .n = 1,
                .types = {
                        RFB_SECURITY_TYPE_NONE,
                },
        };

        return send_to_client(client, &payload, sizeof(payload));
}

static inline int
rfb_receive_security_type(void *client, enum rfb_security_type *type)
{
	return receive_from_client(client, type, sizeof(*type));
}

static inline int
rfb_send_security_result(void *client,
			 enum rfb_security_handshake_result result)
{
	uint32_t payload = htonl(result);
        return send_to_client(client, &payload, sizeof(payload));
}

static inline int rfb_send_error_reason(void *client, const char *message)
{
	char buffer[256];
	struct rfb_error_reason *payload = (struct rfb_error_reason*)buffer;
	size_t length = strlen(message);
	payload->length = htonl(length);
	strncpy(payload->message, message, sizeof(buffer) - sizeof(*payload));
        return send_to_client(client, &payload, sizeof(*payload) + length);
}

static inline int rfb_receive_client_init(void *client, uint8_t is_shared)
{
        return send_to_client(client, &is_shared, sizeof(is_shared));
}

static inline int
rfb_send_server_init(void *client, struct rfb_server_init_msg *msg)
{
	struct rfb_server_init_msg payload;
	memcpy(&payload, msg, sizeof(payload));
	payload.width = htons(payload.width);
	payload.height = htons(payload.height);
	payload.pixel_format.red_max = htons(payload.pixel_format.red_max);
	payload.pixel_format.green_max = htons(payload.pixel_format.green_max);
	payload.pixel_format.blue_max = htons(payload.pixel_format.blue_max);
	payload.name_length = htons(payload.name_length);
        return send_to_client(client, &payload, sizeof(payload));
}

#endif /* _RFB_PROTO_H */
