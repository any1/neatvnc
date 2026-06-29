#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "utf8.h"

static void test_single_utf8_to_latin1(const char* input, const char* expected)
{
	size_t inlen = strlen(input);
	char* latin1 = malloc(UTF8_TO_LATIN1_MAX_SIZE(inlen));
	assert(latin1);

	size_t outlen = utf8_to_latin1(latin1, input, inlen);
	assert(outlen == strlen(latin1));
	assert(strcmp(latin1, expected) == 0);
	free(latin1);
}

static void test_latin_to_utf8_and_back(const char* input, const char* expected)
{
	size_t inlen = strlen(input);
	char* utf8 = malloc(LATIN1_TO_UTF8_MAX_SIZE(inlen));
	assert(utf8);

	size_t outlen = latin1_to_utf8(utf8, input, inlen);
	assert(outlen == strlen(utf8));
	assert(strcmp(utf8, expected) == 0);
	free(utf8);

	test_single_utf8_to_latin1(expected, input);
}

static void test_utf8_to_latin1_n(const char* in, size_t inlen,
		const char* exp, size_t explen)
{
	char* latin1 = malloc(UTF8_TO_LATIN1_MAX_SIZE(inlen));
	assert(latin1);

	size_t outlen = utf8_to_latin1(latin1, in, inlen);
	assert(outlen == explen);
	assert(memcmp(latin1, exp, explen) == 0);
	free(latin1);
}

static void test_malicious_utf8(void)
{
	/* Stray continuation bytes. */
	test_utf8_to_latin1_n("\x80", 1, "?", 1);
	test_utf8_to_latin1_n("\x80\x80\xbf", 3, "???", 3);

	/* Truncated sequences at end of input. */
	test_utf8_to_latin1_n("\xc4", 1, "?", 1);
	test_utf8_to_latin1_n("\xe0\xa0", 2, "?", 1);
	test_utf8_to_latin1_n("\xf0\x90\x80", 3, "?", 1);

	/* Missing continuation byte; decoder must resync and keep the next
	 * byte instead of swallowing it.
	 */
	test_utf8_to_latin1_n("\xc4""A", 2, "?A", 2);
	test_utf8_to_latin1_n("\xc4\xc4\x80", 3, "??", 2);

	/* Overlong encodings must not smuggle a byte through. */
	test_utf8_to_latin1_n("\xc0\x80", 2, "?", 1);
	test_utf8_to_latin1_n("\xc0\xaf", 2, "?", 1);
	test_utf8_to_latin1_n("\xe0\x80\x80", 3, "?", 1);
	test_utf8_to_latin1_n("\xf0\x80\x80\x80", 4, "?", 1);

	/* UTF-16 surrogate (U+D800) and code point beyond U+10FFFF. */
	test_utf8_to_latin1_n("\xed\xa0\x80", 3, "?", 1);
	test_utf8_to_latin1_n("\xf7\xbf\xbf\xbf", 4, "?", 1);

	/* Obsolete 5- and 6-byte lead bytes. */
	test_utf8_to_latin1_n("\xf8\x80\x80\x80\x80", 5, "?????", 5);
	test_utf8_to_latin1_n("\xfc\x80\x80\x80\x80\x80", 6, "??????", 6);

	/* Valid characters around invalid input still decode correctly. */
	test_utf8_to_latin1_n("a\xc3\xa9""b", 4, "a\xe9""b", 3);
}

int main()
{
	test_latin_to_utf8_and_back("", "");
	test_latin_to_utf8_and_back("abc", "abc");
	test_latin_to_utf8_and_back("ABC \xc4\xd6\xdc\xe4\xf6\xfc\xdf",
		"ABC ÄÖÜäöüß");

	test_single_utf8_to_latin1("Hello €!", "Hello ?!");

	test_malicious_utf8();

	return 0;
}
