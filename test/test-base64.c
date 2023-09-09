#include "base64.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static bool test_encode_0(void)
{
	char buf[1] = {};
	base64_encode(buf, (const uint8_t*)"", 0);
	return strlen(buf) == 0;
}

static bool test_encode_1(void)
{
	static const char input[] = "a";
	char buf[BASE64_ENCODED_SIZE(sizeof(input) - 1)] = {};
	base64_encode(buf, (const uint8_t*)input, sizeof(input) - 1);
	return strcmp(buf, "YQ==") == 0;
}

static bool test_encode_2(void)
{
	static const char input[] = "ab";
	char buf[BASE64_ENCODED_SIZE(sizeof(input) - 1)] = {};
	base64_encode(buf, (const uint8_t*)input, sizeof(input) - 1);
	return strcmp(buf, "YWI=") == 0;
}

static bool test_encode_3(void)
{
	static const char input[] = "abc";
	char buf[BASE64_ENCODED_SIZE(sizeof(input) - 1)] = {};
	base64_encode(buf, (const uint8_t*)input, sizeof(input) - 1);
	return strcmp(buf, "YWJj") == 0;
}

static bool test_encode_4(void)
{
	static const char input[] = "abcd";
	char buf[BASE64_ENCODED_SIZE(sizeof(input) - 1)] = {};
	base64_encode(buf, (const uint8_t*)input, sizeof(input) - 1);
	return strcmp(buf, "YWJjZA==") == 0;
}

static bool test_encode_5(void)
{
	static const char input[] = "abcde";
	char buf[BASE64_ENCODED_SIZE(sizeof(input) - 1)] = {};
	base64_encode(buf, (const uint8_t*)input, sizeof(input) - 1);
	return strcmp(buf, "YWJjZGU=") == 0;
}

static bool test_decode_0(void)
{
	uint8_t buf[1] = {};
	ssize_t r = base64_decode(buf, "");
	return r == 0 && buf[0] == 0;
}

static bool test_decode_1(void)
{
	static const char input[] = "YQ==";
	uint8_t buf[BASE64_DECODED_MAX_SIZE(sizeof(input) - 1)] = {};
	ssize_t r = base64_decode(buf, input);
	return r == 1 && memcmp(buf, "a", r) == 0;
}

static bool test_decode_2(void)
{
	static const char input[] = "YWI=";
	uint8_t buf[BASE64_DECODED_MAX_SIZE(sizeof(input) - 1)] = {};
	ssize_t r = base64_decode(buf, input);
	return r == 2 && memcmp(buf, "ab", r) == 0;
}

static bool test_decode_3(void)
{
	static const char input[] = "YWJj";
	uint8_t buf[BASE64_DECODED_MAX_SIZE(sizeof(input) - 1)] = {};
	ssize_t r = base64_decode(buf, input);
	return r == 3 && memcmp(buf, "abc", r) == 0;
}

static bool test_decode_4(void)
{
	static const char input[] = "YWJjZA==";
	uint8_t buf[BASE64_DECODED_MAX_SIZE(sizeof(input) - 1)] = {};
	ssize_t r = base64_decode(buf, input);
	return r == 4 && memcmp(buf, "abcd", r) == 0;
}

static bool test_decode_5(void)
{
	static const char input[] = "YWJjZGU=";
	uint8_t buf[BASE64_DECODED_MAX_SIZE(sizeof(input) - 1)] = {};
	ssize_t r = base64_decode(buf, input);
	return r == 5 && memcmp(buf, "abcde", r) == 0;
}

#define XSTR(s) STR(s)
#define STR(s) #s

#define RUN_TEST(name) ({ \
	bool ok = test_ ## name(); \
	printf("[%s] %s\n", ok ? " OK " : "FAIL", XSTR(name)); \
	ok; \
})

int main()
{
	bool ok = true;

	ok &= RUN_TEST(encode_0);
	ok &= RUN_TEST(encode_1);
	ok &= RUN_TEST(encode_2);
	ok &= RUN_TEST(encode_3);
	ok &= RUN_TEST(encode_4);
	ok &= RUN_TEST(encode_5);

	ok &= RUN_TEST(decode_0);
	ok &= RUN_TEST(decode_1);
	ok &= RUN_TEST(decode_2);
	ok &= RUN_TEST(decode_3);
	ok &= RUN_TEST(decode_4);
	ok &= RUN_TEST(decode_5);

	return ok ? 0 : 1;
}
