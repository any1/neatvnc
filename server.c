#include "rfb-proto.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/queue.h>
#include <assert.h>
#include <uv.h>
#include <libdrm/drm_fourcc.h>

#define READ_BUFFER_SIZE 4096
#define MSG_BUFFER_SIZE 4096

enum vnc_encodings {
	VNC_ENCODING_RAW = 1 << 0,
	VNC_ENCODING_COPYRECT = 1 << 1,
	VNC_ENCODING_RRE = 1 << 2,
	VNC_ENCODING_HEXTILE = 1 << 3,
	VNC_ENCODING_TRLE = 1 << 4,
	VNC_ENCODING_ZRLE = 1 << 5,
	VNC_ENCODING_CURSOR = 1 << 6,
	VNC_ENCODING_DESKTOPSIZE = 1 << 7,
};

enum vnc_client_state {
	VNC_CLIENT_STATE_ERROR = -1,
	VNC_CLIENT_STATE_WAITING_FOR_VERSION = 0,
	VNC_CLIENT_STATE_WAITING_FOR_SECURITY,
	VNC_CLIENT_STATE_WAITING_FOR_INIT,
	VNC_CLIENT_STATE_READY,
};

struct vnc_server;

struct vnc_client {
	uv_tcp_t stream_handle;
	struct vnc_server *server;
	enum vnc_client_state state;
	uint32_t pixfmt;
	enum vnc_encodings encodings;
	LIST_ENTRY(vnc_client) link;
	size_t buffer_index;
	size_t buffer_len;
	uint8_t msg_buffer[MSG_BUFFER_SIZE];
};

struct vnc_write_request {
	uv_write_t request;
	uv_write_cb on_done;
	uv_buf_t buffer;
};

LIST_HEAD(vnc_client_list, vnc_client);

struct vnc_display {
	uint16_t width;
	uint16_t height;
	uint32_t pixfmt; /* fourcc pixel format */
	char *name;
};

struct vnc_server {
	uv_tcp_t tcp_handle;
	struct vnc_client_list clients;
	struct vnc_display display;
};

static const char* fourcc_to_string(uint32_t fourcc)
{
	static char buffer[5];

	buffer[0] = (fourcc >>  0) & 0xff;
	buffer[1] = (fourcc >>  8) & 0xff;
	buffer[2] = (fourcc >> 16) & 0xff;
	buffer[3] = (fourcc >> 24) & 0xff;
	buffer[4] = '\0';

	return buffer;
}

static void on_write_req_done(uv_write_t *req, int status)
{
	struct vnc_write_request *self = (struct vnc_write_request*)req;
	if (self->on_done)
		self->on_done(req, status);
	free(self);
}

static int vnc__write(uv_stream_t *stream, const void *payload, size_t size,
		      uv_write_cb on_done)
{
	struct vnc_write_request *req = calloc(1, sizeof(*req));
	if (!req)
		return -1;

	req->buffer.base = (char*)payload;
	req->buffer.len = size;
	req->on_done = on_done;

	return uv_write(&req->request, stream, &req->buffer, 1,
			on_write_req_done);
}

static void allocate_read_buffer(uv_handle_t *handle, size_t suggested_size,
				 uv_buf_t *buf)
{
	(void)suggested_size;

	buf->base = malloc(READ_BUFFER_SIZE);
	buf->len = buf->base ? READ_BUFFER_SIZE : 0;
}

static void cleanup_client(uv_handle_t* handle)
{
	struct vnc_client *client = (struct vnc_client*)handle;

	LIST_REMOVE(client, link);
	free(client);
}

static void close_after_write(uv_write_t *req, int status)
{
	uv_close((uv_handle_t*)req->handle, cleanup_client);
}

static int handle_unsupported_version(struct vnc_client *client)
{
	char buffer[256];

	client->state = VNC_CLIENT_STATE_ERROR;

	struct rfb_error_reason *reason =
		(struct rfb_error_reason*)(buffer + 1);

	static const char reason_string[] = "Unsupported version\n";

	buffer[0] = 0; /* Number of security types is 0 on error */
	reason->length = htonl(strlen(reason_string));
	(void)strcmp(reason->message, reason_string);

	vnc__write((uv_stream_t*)&client->stream_handle, buffer,
		   1 + sizeof(*reason) + strlen(reason_string),
		   close_after_write);

	return 0;
}

static int on_version_message(struct vnc_client *client)
{
	if (client->buffer_len - client->buffer_index < 12)
		return 0;

	char version_string[13];
	memcpy(version_string, client->msg_buffer + client->buffer_index, 12);
	version_string[12] = '\0';

	if (strcmp(RFB_VERSION_MESSAGE, version_string) != 0)
		return handle_unsupported_version(client);

	const static struct rfb_security_types_msg security = {
		.n = 1,
		.types = {
			RFB_SECURITY_TYPE_NONE,
		},
	};

	vnc__write((uv_stream_t*)&client->stream_handle, &security, sizeof(security), NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_SECURITY;
	return 12;
}

static int handle_invalid_security_type(struct vnc_client *client)
{
	char buffer[256];

	client->state = VNC_CLIENT_STATE_ERROR;

	uint8_t *result = (uint8_t*)buffer;

	struct rfb_error_reason *reason =
		(struct rfb_error_reason*)(buffer + sizeof(*result));

	static const char reason_string[] = "Unsupported version\n";

	*result = htonl(RFB_SECURITY_HANDSHAKE_FAILED);
	reason->length = htonl(strlen(reason_string));
	(void)strcmp(reason->message, reason_string);

	vnc__write((uv_stream_t*)&client->stream_handle, buffer,
		   sizeof(*result) + sizeof(*reason) + strlen(reason_string),
		   close_after_write);

	return 0;
}

static int on_security_message(struct vnc_client *client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t type = client->msg_buffer[client->buffer_index];

	if (type != RFB_SECURITY_TYPE_NONE)
		return handle_invalid_security_type(client);

	enum rfb_security_handshake_result result
		= htonl(RFB_SECURITY_HANDSHAKE_OK);

	vnc__write((uv_stream_t*)&client->stream_handle, &result, sizeof(result), NULL);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_INIT;
	return sizeof(type);
}

static void disconnect_all_other_clients(struct vnc_client *client)
{
	// TODO
}

int rfb_pixfmt_from_fourcc(struct rfb_pixel_format *dst, uint32_t src) {
	switch (src & ~DRM_FORMAT_BIG_ENDIAN) {
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		dst->red_shift = 24;
		dst->green_shift = 16;
		dst->blue_shift = 8;
		goto bpp_32;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		dst->red_shift = 8;
		dst->green_shift = 16;
		dst->blue_shift = 24;
		goto bpp_32;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		dst->red_shift = 0;
		dst->green_shift = 8;
		dst->blue_shift = 16;
		goto bpp_32;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		dst->red_shift = 16;
		dst->green_shift = 8;
		dst->blue_shift = 0;
bpp_32:
		dst->bits_per_pixel = 32;
		dst->depth = 24;
		dst->red_max = htons(0xff);
		dst->green_max = htons(0xff);
		dst->blue_max = htons(0xff);
		break;
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX4444:
		dst->red_shift = 12;
		dst->green_shift = 8;
		dst->blue_shift = 4;
		goto bpp_16;
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX4444:
		dst->red_shift = 4;
		dst->green_shift = 8;
		dst->blue_shift = 12;
		goto bpp_16;
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_XRGB4444:
		dst->red_shift = 8;
		dst->green_shift = 4;
		dst->blue_shift = 0;
		goto bpp_16;
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR4444:
		dst->red_shift = 0;
		dst->green_shift = 4;
		dst->blue_shift = 8;
bpp_16:
		dst->bits_per_pixel = 16;
		dst->depth = 12;
		dst->red_max = htons(0x7f);
		dst->green_max = htons(0x7f);
		dst->blue_max = htons(0x7f);
		break;
	default:
		return -1;
	}

	dst->big_endian_flag = !!(src & DRM_FORMAT_BIG_ENDIAN);
	dst->true_colour_flag = 1;

	return 0;
};

static int max_values_to_depth(int r, int g, int b)
{
	if (r ==    5 && g ==    5 && b ==    3) return  8;
	if (r ==   15 && g ==   15 && b ==   15) return 12;
	if (r ==   31 && g ==   31 && b ==   31) return 15;
	if (r ==   31 && g ==  127 && b ==   31) return 16;
	if (r ==  255 && g ==  255 && b ==  255) return 24;
	if (r == 1023 && g == 1023 && b == 1023) return 30;
	return -1;
}

static uint32_t shift_values_to_fourcc(int r, int g, int b, int bpp)
{
#define RGBEQ(rv, gv, bv) (r == (rv) && g == (gv) && b == (bv))
	if (bpp == 32 && RGBEQ(24, 16,  8)) return DRM_FORMAT_RGBX8888;
	if (bpp == 32 && RGBEQ( 8, 16, 24)) return DRM_FORMAT_BGRX8888;
	if (bpp == 32 && RGBEQ(16,  8,  0)) return DRM_FORMAT_XRGB8888;
	if (bpp == 32 && RGBEQ( 0,  8, 16)) return DRM_FORMAT_XBGR8888;

	if (bpp == 32 && RGBEQ(22, 12,  2)) return DRM_FORMAT_RGBX1010102;
	if (bpp == 32 && RGBEQ( 2, 12, 22)) return DRM_FORMAT_BGRX1010102;
	if (bpp == 32 && RGBEQ(20, 10,  0)) return DRM_FORMAT_XRGB2101010;
	if (bpp == 32 && RGBEQ( 0, 10, 20)) return DRM_FORMAT_XBGR2101010;

	if (bpp == 24 && RGBEQ( 0,  8, 16)) return DRM_FORMAT_BGR888;
	if (bpp == 24 && RGBEQ(16,  8,  0)) return DRM_FORMAT_RGB888;

	if (bpp == 16 && RGBEQ(12,  8,  4)) return DRM_FORMAT_RGBX4444;
	if (bpp == 16 && RGBEQ( 4,  8, 12)) return DRM_FORMAT_BGRX4444;
	if (bpp == 16 && RGBEQ( 8,  4,  0)) return DRM_FORMAT_XRGB4444;
	if (bpp == 16 && RGBEQ( 0,  4,  8)) return DRM_FORMAT_XBGR4444;

	if (bpp == 16 && RGBEQ(11,  6,  1)) return DRM_FORMAT_RGBX5551;
	if (bpp == 16 && RGBEQ( 1,  6, 11)) return DRM_FORMAT_BGRX5551;
	if (bpp == 16 && RGBEQ(15,  5,  0)) return DRM_FORMAT_XRGB1555;
	if (bpp == 16 && RGBEQ( 0,  5, 15)) return DRM_FORMAT_XBGR1555;

	if (bpp == 16 && RGBEQ(11,  5,  0)) return DRM_FORMAT_RGB565;
	if (bpp == 16 && RGBEQ( 0,  5, 11)) return DRM_FORMAT_BGR565;

	if (bpp ==  8 && RGBEQ( 5,  2,  0)) return DRM_FORMAT_RGB332;
	if (bpp ==  8 && RGBEQ( 0,  2,  5)) return DRM_FORMAT_BGR233;

	return DRM_FORMAT_INVALID;
#undef RGBEQ
}

int get_fourcc_depth(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGB332:
	case DRM_FORMAT_BGR233:
		return 8;
	default:
		return (((fourcc >> 24) & 0xff) - '0') +
		       (((fourcc >> 16) & 0xff) - '0') * 10;
	}
}

/* Note: Pixel format is in network order */
uint32_t rfb_pixfmt_to_fourcc(const struct rfb_pixel_format *fmt)
{
	if (!fmt->true_colour_flag)
		return DRM_FORMAT_INVALID;

	uint16_t red_max = ntohs(fmt->red_max);
	uint16_t green_max = ntohs(fmt->green_max);
	uint16_t blue_max = ntohs(fmt->blue_max);

	/* Note: The depth value given by the client is ignored */
	int depth = max_values_to_depth(red_max, green_max, blue_max);
	if (depth < 0)
		return DRM_FORMAT_INVALID;

	uint32_t fourcc =
		shift_values_to_fourcc(fmt->red_shift, fmt->green_shift,
				       fmt->blue_shift, fmt->bits_per_pixel);

	if (fourcc == DRM_FORMAT_INVALID)
		return DRM_FORMAT_INVALID;

	if (get_fourcc_depth(fourcc) != depth)
		return DRM_FORMAT_INVALID;

	fourcc |= fmt->big_endian_flag ? DRM_FORMAT_BIG_ENDIAN : 0;

	return fourcc;
}

static void send_server_init_message(struct vnc_client *client)
{
	struct vnc_server *server = client->server;
	struct vnc_display *display = &server->display;

	size_t name_len = strlen(display->name);
	size_t size = sizeof(struct rfb_server_init_msg) + name_len;

	struct rfb_server_init_msg *msg = calloc(1, size);
	if (!msg) {
		uv_close((uv_handle_t*)&client->stream_handle, cleanup_client);
		return;
	}

	msg->width = htons(display->width),
	msg->height = htons(display->height),
	msg->name_length = htonl(name_len),
	memcpy(msg->name_string, display->name, name_len);
	rfb_pixfmt_from_fourcc(&msg->pixel_format, display->pixfmt);

	vnc__write((uv_stream_t*)&client->stream_handle, msg, size, NULL);

	free(msg);
}

static int on_init_message(struct vnc_client *client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	uint8_t shared_flag = client->msg_buffer[client->buffer_index];
	if (shared_flag)
		disconnect_all_other_clients(client);

	send_server_init_message(client);

	client->state = VNC_CLIENT_STATE_READY;
	return sizeof(shared_flag);
}

static int on_client_set_pixel_format(struct vnc_client *client)
{
	if (client->buffer_len - client->buffer_index
			< 4 + sizeof(struct rfb_pixel_format))
		return 0;

	struct rfb_pixel_format *fmt =
		(struct rfb_pixel_format*)(client->msg_buffer +
				client->buffer_index + 4);

	if (!fmt->true_colour_flag) {
		/* We don't really know what to do with color maps right now */
		uv_close((uv_handle_t*)&client->stream_handle, cleanup_client);
		return 0;
	}

	client->pixfmt = rfb_pixfmt_to_fourcc(fmt);

	printf("SetPixelFormat: %s\n", fourcc_to_string(client->pixfmt));

	return 4 + sizeof(struct rfb_pixel_format);
}

static int on_client_set_encodings(struct vnc_client *client)
{
	struct rfb_client_set_encodings_msg *msg =
		(struct rfb_client_set_encodings_msg*)(client->msg_buffer +
				client->buffer_index);

	int n_encodings = ntohs(msg->n_encodings);

	uint32_t e = 0;

	for (int i = 0; i < n_encodings; ++i)
		switch (msg->encodings[i]) {
		case RFB_ENCODING_RAW: e |= VNC_ENCODING_RAW;
		case RFB_ENCODING_COPYRECT: e |= VNC_ENCODING_COPYRECT;
		case RFB_ENCODING_RRE: e |= VNC_ENCODING_RRE;
		case RFB_ENCODING_HEXTILE: e |= VNC_ENCODING_HEXTILE;
		case RFB_ENCODING_TRLE: e |= VNC_ENCODING_TRLE;
		case RFB_ENCODING_ZRLE: e |= VNC_ENCODING_ZRLE;
		case RFB_ENCODING_CURSOR: e |= VNC_ENCODING_CURSOR;
		case RFB_ENCODING_DESKTOPSIZE: e |= VNC_ENCODING_COPYRECT;
		}

	client->encodings = e;

	return sizeof(*msg) + 4 * n_encodings;
}

static int on_client_fb_update_request(struct vnc_client *client)
{
	struct rfb_client_fb_update_req_msg *msg =
		(struct rfb_client_fb_update_req_msg*)(client->msg_buffer +
				client->buffer_index);

	int incremental = msg->incremental;
	int x = ntohs(msg->x);
	int y = ntohs(msg->y);
	int width = ntohs(msg->width);
	int height = ntohs(msg->height);

	printf("framebuffer update: %d, %d. %d %d\n", x, y, width, height);
	// TODO

	return sizeof(*msg);
}

static int on_client_key_event(struct vnc_client *client)
{
	struct rfb_client_key_event_msg *msg =
		(struct rfb_client_key_event_msg*)(client->msg_buffer +
				client->buffer_index);

	int down_flag = msg->down_flag;
	uint32_t key = ntohl(msg->key);

	printf("key event: %d\n", key);
	// TODO

	return sizeof(*msg);
}

static int on_client_pointer_event(struct vnc_client *client)
{
	struct rfb_client_pointer_event_msg *msg =
		(struct rfb_client_pointer_event_msg*)(client->msg_buffer +
				client->buffer_index);

	int button_mask = msg->button_mask;
	uint16_t x = ntohs(msg->x);
	uint16_t y = ntohs(msg->y);

	printf("pointer event: %d, %d, %d\n", x, y, button_mask);
	// TODO

	return sizeof(*msg);
}

static int on_client_cut_text(struct vnc_client *client)
{
	struct rfb_client_cut_text_msg *msg =
		(struct rfb_client_cut_text_msg*)(client->msg_buffer +
				client->buffer_index);

	uint32_t length = ntohl(msg->length);

	// TODO

	return sizeof(*msg) + length;
}

static int on_client_message(struct vnc_client *client)
{
	if (client->buffer_len - client->buffer_index < 1)
		return 0;

	enum rfb_client_to_server_msg_type type =
		client->msg_buffer[client->buffer_index];

	switch (type) {
	case RFB_CLIENT_TO_SERVER_SET_PIXEL_FORMAT:
		return on_client_set_pixel_format(client);
	case RFB_CLIENT_TO_SERVER_SET_ENCODINGS:
		return on_client_set_encodings(client);
	case RFB_CLIENT_TO_SERVER_FRAMEBUFFER_UPDATE_REQUEST:
		return on_client_fb_update_request(client);
	case RFB_CLIENT_TO_SERVER_KEY_EVENT:
		return on_client_key_event(client);
	case RFB_CLIENT_TO_SERVER_POINTER_EVENT:
		return on_client_pointer_event(client);
	case RFB_CLIENT_TO_SERVER_CLIENT_CUT_TEXT:
		return on_client_cut_text(client);
	}

	uv_close((uv_handle_t*)&client->stream_handle, cleanup_client);
	return 0;
}

static int try_read_client_message(struct vnc_client *client)
{
	printf("Client state: %d\n", client->state);
	switch (client->state) {
	case VNC_CLIENT_STATE_ERROR:
		uv_close((uv_handle_t*)&client->stream_handle, cleanup_client);
		return 0;
	case VNC_CLIENT_STATE_WAITING_FOR_VERSION:
		return on_version_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_SECURITY:
		return on_security_message(client);
	case VNC_CLIENT_STATE_WAITING_FOR_INIT:
		return on_init_message(client);
	case VNC_CLIENT_STATE_READY:
		return on_client_message(client);
	}

	abort();
	return 0;
}

static void on_client_read(uv_stream_t *stream, ssize_t n_read,
			   const uv_buf_t *buf)
{
	if (n_read == UV_EOF) {
		// TODO: Make it known to the user of the library that the
		// client is gone.
		uv_close((uv_handle_t*)stream, cleanup_client);
		return;
	}

	if (n_read < 0)
		return;

	struct vnc_client *client = (struct vnc_client*)stream;

	assert(client->buffer_index == 0);

	if (n_read > MSG_BUFFER_SIZE - client->buffer_len) {
		/* Can't handle this. Let's just give up */
		client->state = VNC_CLIENT_STATE_ERROR;
		uv_close((uv_handle_t*)stream, cleanup_client);
		return;
	}

	memcpy(client->msg_buffer + client->buffer_len, buf->base, n_read);
	client->buffer_len += n_read;

	while (1) {
		int rc = try_read_client_message(client);
		if (rc == 0)
			break;

		client->buffer_index += rc;
	}

	assert(client->buffer_index <= client->buffer_len);

	memmove(client->msg_buffer, client->msg_buffer + client->buffer_index,
		client->buffer_index);
	client->buffer_len -= client->buffer_index;
	client->buffer_index = 0;
}

static void on_connection(uv_stream_t *server_stream, int status)
{
	struct vnc_server *server = (struct vnc_server*)server_stream;

	struct vnc_client *client = calloc(1, sizeof(*client));
	if (!client)
		return;

	client->server = server;

	uv_tcp_init(uv_default_loop(), &client->stream_handle);

	uv_accept((uv_stream_t*)&server->tcp_handle,
		  (uv_stream_t*)&client->stream_handle);

	uv_read_start((uv_stream_t*)&client->stream_handle,
		      allocate_read_buffer, on_client_read);

	vnc__write((uv_stream_t*)&client->stream_handle, RFB_VERSION_MESSAGE,
		   strlen(RFB_VERSION_MESSAGE), NULL);

	LIST_INSERT_HEAD(&server->clients, client, link);

	client->state = VNC_CLIENT_STATE_WAITING_FOR_VERSION;
}

int vnc_server_init(struct vnc_server *self, const char* address, int port)
{
	LIST_INIT(&self->clients);

	uv_tcp_init(uv_default_loop(), &self->tcp_handle);

	struct sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(address);
	addr.sin_port = htons(port);

	if (uv_tcp_bind(&self->tcp_handle, (const struct sockaddr*)&addr, 0) < 0)
		goto failure;

	if (uv_listen((uv_stream_t*)&self->tcp_handle, 16, on_connection) < 0)
		goto failure;

	return 0;

failure:
	uv_unref((uv_handle_t*)&self->tcp_handle);
	return -1;
}

int main()
{
	struct vnc_server server = { 0 };
	server.display.pixfmt = DRM_FORMAT_RGBX8888;
	server.display.width = 1024;
	server.display.height = 768;
	server.display.name = "Silly VNC";

	vnc_server_init(&server, "127.0.0.1", 5900);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}
