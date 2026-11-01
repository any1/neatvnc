#include "tst.h"
#include "stream/http.h"

static int test_get_empty_path()
{
	const char* text = "GET / HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(strlen(text) - 4, req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_get_one_elem_path()
{
	const char* text = "GET /foo HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(strlen(text) - 4, req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_get_two_elem_path()
{
	const char* text = "GET /foo/bar HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(strlen(text) - 4, req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_put_empty_path()
{
	const char* text = "PUT / HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_content_length()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"Content-Length: 42\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(42, req.content_length);
	ASSERT_UINT_EQ(strlen(text), req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_get_with_negative_content_length()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"Content-Length: -1\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_overflowing_content_length()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"Content-Length: 99999999999999999999999\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_non_numeric_content_length()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"Content-Length: 42foo\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_content_type()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"Content-Type: foo/bar\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_STR_EQ("foo/bar", req.content_type);
	ASSERT_UINT_EQ(strlen(text), req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_get_with_too_many_fields()
{
	char text[4096] = "GET / HTTP/1.1\r\n";
	for (int i = 0; i < HTTP_FIELD_INDEX_MAX + 1; ++i)
		strcat(text, "foo: bar\r\n");
	strcat(text, "\r\n");

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_without_header_terminator()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"foo: bar\r\n";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_truncated_field_value()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"foo: bar\r\n"
	"asdf: xy";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_truncated_field_key()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"foo: bar\r\n"
	"as";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_malformed_field()
{
	const char* text =
	"GET / HTTP/1.1\r\n"
	"foo bar\r\n"
	"\r\n";

	struct http_req req;
	ASSERT_INT_LT(0, http_req_parse(&req, text));

	return 0;
}

static int test_get_with_single_query()
{
	const char* text = "GET /path?key=value HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(strlen(text) - 4, req.header_length);

	http_req_free(&req);
	return 0;
}

static int test_get_with_two_query_pairs()
{
	const char* text = "GET /path?foo=bar&asdf=xyz HTTP/1.1\r\n\r\nasdf";

	struct http_req req;
	ASSERT_INT_EQ(0, http_req_parse(&req, text));

	ASSERT_UINT_EQ(strlen(text) - 4, req.header_length);

	http_req_free(&req);
	return 0;
}

int main()
{
	int r = 0;
	RUN_TEST(test_get_empty_path);
	RUN_TEST(test_get_one_elem_path);
	RUN_TEST(test_get_two_elem_path);
	RUN_TEST(test_put_empty_path);
	RUN_TEST(test_get_with_content_length);
	RUN_TEST(test_get_with_negative_content_length);
	RUN_TEST(test_get_with_overflowing_content_length);
	RUN_TEST(test_get_with_non_numeric_content_length);
	RUN_TEST(test_get_with_content_type);
	RUN_TEST(test_get_with_too_many_fields);
	RUN_TEST(test_get_without_header_terminator);
	RUN_TEST(test_get_with_truncated_field_value);
	RUN_TEST(test_get_with_truncated_field_key);
	RUN_TEST(test_get_with_malformed_field);
	RUN_TEST(test_get_with_single_query);
	RUN_TEST(test_get_with_two_query_pairs);
	return r;
}
