/*
 * Minimal neatvnc test server for RFB handshake tests.
 *
 * Usage:
 *   rfb-test-server --port PORT --auth-mode none|des|vencrypt --password PASS
 *   rfb-test-server --encrypt-challenge HEX --password PASS
 *
 * In server mode: starts listening, prints "READY <port>\n" to stdout,
 * runs until SIGTERM.
 *
 * In encrypt mode: DES-encrypts the 16-byte hex challenge with the
 * password and prints the hex response to stdout, then exits.
 */

#include <neatvnc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <aml.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

#include "crypto/des-rfb.h"

static const char* auth_password = NULL;
static const char* auth_username = NULL;

static bool on_auth(const struct nvnc_auth_creds* creds, void* ud)
{
	(void)ud;
	if (auth_username) {
		const char* u = nvnc_auth_creds_get_username(creds);
		if (!u || strcmp(u, auth_username) != 0)
			return false;
	}
	return nvnc_auth_creds_verify(creds, auth_password);
}

static void on_sigterm(struct aml_signal* sig)
{
	(void)sig;
	aml_exit(aml_get_default());
}

static int hex_to_bytes(const char* hex, uint8_t* out, size_t out_len)
{
	size_t hex_len = strlen(hex);
	if (hex_len != out_len * 2)
		return -1;
	for (size_t i = 0; i < out_len; i++) {
		unsigned int byte;
		if (sscanf(hex + i * 2, "%02x", &byte) != 1)
			return -1;
		out[i] = byte;
	}
	return 0;
}

int main(int argc, char* argv[])
{
	const char* port_str = NULL;
	const char* auth_mode = "none";
	const char* encrypt_challenge = NULL;
	const char* tls_cert = NULL;
	const char* tls_key = NULL;

	static const struct option long_options[] = {
		{ "port", required_argument, NULL, 'p' },
		{ "auth-mode", required_argument, NULL, 'a' },
		{ "password", required_argument, NULL, 'P' },
		{ "encrypt-challenge", required_argument, NULL, 'e' },
		{ "tls-cert", required_argument, NULL, 'c' },
		{ "tls-key", required_argument, NULL, 'k' },
		{ "username", required_argument, NULL, 'u' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
		switch (opt) {
		case 'p': port_str = optarg; break;
		case 'a': auth_mode = optarg; break;
		case 'P': auth_password = optarg; break;
		case 'e': encrypt_challenge = optarg; break;
		case 'c': tls_cert = optarg; break;
		case 'k': tls_key = optarg; break;
		case 'u': auth_username = optarg; break;
		default:
			return 1;
		}
	}

	/* Encrypt mode: just compute DES response and exit */
	if (encrypt_challenge) {
		if (!auth_password) {
			fprintf(stderr, "--encrypt-challenge requires --password\n");
			return 1;
		}
		uint8_t challenge[16], response[16];
		if (hex_to_bytes(encrypt_challenge, challenge, 16) < 0) {
			fprintf(stderr, "Invalid challenge hex\n");
			return 1;
		}
		crypto_des_rfb_encrypt(response, challenge, auth_password);
		for (int i = 0; i < 16; i++)
			printf("%02x", response[i]);
		printf("\n");
		return 0;
	}

	/* Server mode */
	if (!port_str) {
		fprintf(stderr, "Usage: rfb-test-server --port PORT "
				"--auth-mode none|des|vencrypt "
				"--password PASS [--username USER] "
				"[--tls-cert CERT --tls-key KEY]\n");
		return 1;
	}

	int port = atoi(port_str);

	struct aml* aml = aml_new();
	aml_set_default(aml);

	struct nvnc* server = nvnc_new();
	assert(server);

	int rc = nvnc_listen_tcp(server, "127.0.0.1", port,
			NVNC_STREAM_NORMAL);
	if (rc != 0) {
		fprintf(stderr, "Failed to listen on port %d\n", port);
		return 1;
	}

	if (strcmp(auth_mode, "des") == 0) {
		if (!auth_password) {
			fprintf(stderr, "--auth-mode des requires --password\n");
			return 1;
		}
		rc = nvnc_enable_auth(server,
				NVNC_AUTH_REQUIRE_AUTH |
				NVNC_AUTH_ALLOW_BROKEN_CRYPTO,
				on_auth, NULL);
		assert(rc == 0);
	} else if (strcmp(auth_mode, "vencrypt") == 0) {
		if (!auth_password) {
			fprintf(stderr, "--auth-mode vencrypt requires --password\n");
			return 1;
		}
		if (!tls_cert || !tls_key) {
			fprintf(stderr, "--auth-mode vencrypt requires --tls-cert and --tls-key\n");
			return 1;
		}
		rc = nvnc_set_tls_creds(server, tls_key, tls_cert);
		if (rc != 0) {
			fprintf(stderr, "Failed to set TLS credentials\n");
			return 1;
		}
		rc = nvnc_enable_auth(server, NVNC_AUTH_REQUIRE_AUTH,
				on_auth, NULL);
		assert(rc == 0);
	}

	struct nvnc_display* display = nvnc_display_new(0, 0);
	assert(display);
	nvnc_add_display(server, display);
	nvnc_set_name(server, "test");

	struct nvnc_frame* fb = nvnc_frame_new(64, 64, DRM_FORMAT_RGBX8888, 64);
	assert(fb);
	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, 64, 64);
	nvnc_display_feed_buffer(display, fb, &damage);
	pixman_region_fini(&damage);

	/* SIGTERM handler for clean shutdown */
	struct aml_signal* sig = aml_signal_new(SIGTERM, on_sigterm,
			NULL, NULL);
	assert(sig);
	aml_start(aml, sig);
	aml_unref(sig);

	printf("READY %d\n", port);
	fflush(stdout);

	aml_run(aml);

	nvnc_del(server);
	nvnc_display_unref(display);
	nvnc_frame_unref(fb);
	aml_unref(aml);

	return 0;
}
