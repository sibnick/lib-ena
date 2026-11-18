/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * TechEmpower-style HTTP endpoints for the httpreply samples.
 *
 * This header holds the request parser and the response builders
 * for the /plaintext and /json test types. The unikernel app and
 * the host test harness include the same file, so both run one
 * parser and one response path. [Ticket e1e4844702] [Ticket 42d43d501d]
 *
 * The code is plain C. It has no unikraft dependency, so it also
 * builds on the host. nolibc has no gmtime or strftime, so the
 * date renderer here computes the calendar itself.
 *
 * Spec rules followed (TFB Framework Tests Overview, 2026-11-17):
 * 1. /plaintext body is "Hello, World!" with no trailing newline.
 * 2. /json body is {"message":"Hello, World!"} with key case exactly
 *    "message". The body string is built in the call, not cached.
 * 3. Each response is composed per request. The body buffer is
 *    reused. No headers-plus-body buffer is pre-rendered.
 * 4. Every response carries Server, Date, and Content-Length.
 *
 * Settled Content-Type literals: text/plain and application/json,
 * with no charset parameter. Evidence [Ticket 21214f6a58]:
 * 1. The official TFB verifier (FrameworkBenchmarks master, file
 *    toolset/test_types/verifications.py, function verify_headers,
 *    checked 2026-11-18) matches Content-Type against these full
 *    regexes:
 *      plaintext: ^text/plain(; ?charset=(UTF|utf)-8)?$
 *      json:      ^application/json(; ?charset=(UTF|utf)-8)?$
 *    The charset suffix is optional, so the bare form passes.
 * 2. The wiki REQUIREMENT text states the bare form ("The response
 *    content type must be set to text/plain"). Only the example
 *    response shows a charset.
 * 3. The bare form sends fewer bytes per response, which serves the
 *    benchmark goal.
 */

#ifndef TFB_HTTP_H
#define TFB_HTTP_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TFB_SERVER_NAME "unikraft-httpreply"
#define TFB_MAX_URI 128
#define TFB_MAX_RESP 320
#define TFB_DATE_LEN 29
#define TFB_MAX_HEAD 2048

static const char tfb_plain_body[] = "Hello, World!";
static const char tfb_404_body[] = "Not Found";

/* Route chosen from the request URI. */
enum tfb_route {
	TFB_ROUTE_LEGACY = 0, /* "/" keeps the legacy benchmark reply */
	TFB_ROUTE_PLAIN,      /* "/plaintext" */
	TFB_ROUTE_JSON,	      /* "/json" */
	TFB_ROUTE_NOT_FOUND,
};

/* Result of parsing one request. */
struct tfb_request {
	enum tfb_route route;
	int keep_alive; /* 0 when the request asks to close */
};

/* Case-insensitive compare of one byte. */
static inline int tfb_ci_eq(char a, char b)
{
	if (a >= 'A' && a <= 'Z')
		a += 'a' - 'A';
	if (b >= 'A' && b <= 'Z')
		b += 'a' - 'A';
	return a == b;
}

/* Return 1 when needle (lower-case) appears in buf[0..len). The
 * scan is fully case-insensitive. */
static inline int tfb_contains_ci(const char *buf, size_t len,
				  const char *needle)
{
	size_t n = strlen(needle);
	size_t i, j;

	if (n == 0 || len < n)
		return 0;
	for (i = 0; i + n <= len; i++) {
		if (buf[i] != 'c' && buf[i] != 'C')
			continue;
		for (j = 0; j < n; j++)
			if (!tfb_ci_eq(buf[i + j], needle[j]))
				break;
		if (j == n)
			return 1;
	}
	return 0;
}

/* Find the end of the header block. Return the byte count up to and
 * including the blank line, or (size_t)-1 when the block is not yet
 * complete. */
static inline size_t tfb_head_end(const char *buf, size_t len)
{
	size_t i;

	if (len < 4)
		return (size_t)-1;
	for (i = 0; i + 4 <= len; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' &&
		    buf[i + 2] == '\r' && buf[i + 3] == '\n')
			return i + 4;
	return (size_t)-1;
}

/* Return 1 when uri equals path exactly. */
static inline int tfb_uri_eq(const char *uri, size_t urilen, const char *path)
{
	size_t plen = strlen(path);

	return urilen == plen && memcmp(uri, path, plen) == 0;
}

/* Parse one request at the start of buf. Return values:
 * >0: the byte count that this request occupies
 *  0: more data is needed before the request is complete
 * -1: malformed request, the caller must close the connection
 */
static inline int tfb_parse_request(const char *buf, size_t len,
				    struct tfb_request *req)
{
	size_t end = tfb_head_end(buf, len);
	size_t i, urilen;
	const char *sp1, *sp2, *uri;
	const char *q;

	if (end == (size_t)-1)
		return len >= TFB_MAX_HEAD ? -1 : 0;

	/* The request line must hold a method, a URI, and a version. */
	for (i = 0; i + 1 < end; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n')
			break;
	if (i + 1 >= end)
		return -1;

	sp1 = memchr(buf, ' ', i);
	if (!sp1 || (size_t)(sp1 - buf) == 0)
		return -1;
	sp2 = memchr(sp1 + 1, ' ', i - (size_t)(sp1 - buf) - 1);
	if (!sp2)
		return -1;

	uri = sp1 + 1;
	urilen = (size_t)(sp2 - uri);
	if (urilen == 0 || urilen > TFB_MAX_URI)
		return -1;

	/* Drop any query string before routing. */
	q = memchr(uri, '?', urilen);
	if (q)
		urilen = (size_t)(q - uri);

	if (tfb_uri_eq(uri, urilen, "/plaintext"))
		req->route = TFB_ROUTE_PLAIN;
	else if (tfb_uri_eq(uri, urilen, "/json"))
		req->route = TFB_ROUTE_JSON;
	else if (tfb_uri_eq(uri, urilen, "/"))
		req->route = TFB_ROUTE_LEGACY;
	else
		req->route = TFB_ROUTE_NOT_FOUND;

	/* Same close rule as the legacy path: close only when the
	 * request says Connection: close. */
	req->keep_alive =
	    tfb_contains_ci(buf, end, "connection: close") ? 0 : 1;

	return (int)end;
}

/* Write the header block shared by every composed response. Return
 * the byte count written, or -1 when buf is too small. */
static inline int tfb_write_head(char *buf, size_t bufsz, const char *status,
				 const char *date, const char *ctype,
				 unsigned int blen)
{
	int n;

	n = snprintf(buf, bufsz,
		     "HTTP/1.1 %s\r\n"
		     "Server: " TFB_SERVER_NAME "\r\n"
		     "Date: %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %u\r\n"
		     "\r\n",
		     status, date, ctype, blen);
	if (n < 0 || (size_t)n >= bufsz)
		return -1;
	return n;
}

/* Build the /plaintext response into buf. The body buffer is shared,
 * the headers are composed per request. Return the total byte count,
 * or -1 when buf is too small. */
static inline int tfb_compose_plain(char *buf, size_t bufsz, const char *date)
{
	size_t blen = sizeof(tfb_plain_body) - 1;
	int h = tfb_write_head(buf, bufsz, "200 OK", date, "text/plain",
			       (unsigned int)blen);

	if (h < 0 || (size_t)h + blen > bufsz)
		return -1;
	memcpy(buf + h, tfb_plain_body, blen);
	return h + (int)blen;
}

/* Build the /json response into buf. The body string is serialized
 * from its parts on each call, not cached across requests. Return
 * the total byte count, or -1 when buf is too small. */
static inline int tfb_compose_json(char *buf, size_t bufsz, const char *date)
{
	static const char key[] = "message";
	static const char msg[] = "Hello, World!";
	char body[64];
	int blen;
	int h;

	blen = snprintf(body, sizeof(body), "{\"%s\":\"%s\"}", key, msg);
	if (blen < 0 || (size_t)blen >= sizeof(body))
		return -1;

	h = tfb_write_head(buf, bufsz, "200 OK", date, "application/json",
			   (unsigned int)blen);
	if (h < 0 || (size_t)h + blen > bufsz)
		return -1;
	memcpy(buf + h, body, (size_t)blen);
	return h + blen;
}

/* Build the 404 response into buf. Return the total byte count, or
 * -1 when buf is too small. */
static inline int tfb_compose_404(char *buf, size_t bufsz, const char *date)
{
	size_t blen = sizeof(tfb_404_body) - 1;
	int h = tfb_write_head(buf, bufsz, "404 Not Found", date, "text/plain",
			       (unsigned int)blen);

	if (h < 0 || (size_t)h + blen > bufsz)
		return -1;
	memcpy(buf + h, tfb_404_body, blen);
	return h + (int)blen;
}

/* Write the RFC 1123 GMT date for the given seconds since the Unix
 * epoch. out must hold TFB_DATE_LEN + 1 bytes. Seconds before the
 * epoch clamp to the epoch. The calendar math follows the
 * civil_from_days algorithm by Howard Hinnant. Return the written
 * string length, which is always TFB_DATE_LEN. */
static inline int tfb_render_date(char *out, size_t outsz, long long secs)
{
	static const char *const wd[7] = {"Sun", "Mon", "Tue", "Wed",
					  "Thu", "Fri", "Sat"};
	static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr",
					   "May", "Jun", "Jul", "Aug",
					   "Sep", "Oct", "Nov", "Dec"};
	long long days, z, era, doe, yoe, year, doy, mp, d, m;
	long long sod;

	if (secs < 0)
		secs = 0;
	days = secs / 86400;
	sod = secs % 86400;

	z = days + 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	year = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp + (mp < 10 ? 3 : -9);
	if (m <= 2)
		year++;

	if (outsz <= TFB_DATE_LEN)
		return -1;

	return snprintf(out, outsz, "%s, %02d %s %04d %02d:%02d:%02d GMT",
			wd[(int)((days + 4) % 7)], (int)d, mo[(int)m - 1],
			(int)year, (int)(sod / 3600), (int)((sod / 60) % 60),
			(int)(sod % 60));
}

#endif /* TFB_HTTP_H */