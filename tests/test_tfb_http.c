/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 *
 * Host test and local server for the TFB-style endpoints in
 * samples/httpreply-mc/tfb_http.h.
 *
 * Default mode runs unit tests: date rendering, exact response
 * bytes, and request parsing with split and pipelined reads.
 *
 * Serve mode listens on a local TCP port and answers /plaintext,
 * /json, and / like the unikernel app. The unikernel needs the
 * ENA device, which QEMU does not model, so this harness lets the
 * compliance validator (samples/httpreply-mc/scripts/tfb_validate.sh)
 * exercise the same parser and response code on the host.
 *
 * The exact-byte tests below pin the settled bare Content-Type
 * literals (text/plain, application/json). The official TFB
 * verifier regex makes the charset suffix optional, so the bare
 * form is spec-valid. See the note in tfb_http.h. [Ticket
 * 21214f6a58]
 *
 * [Ticket e1e4844702] [Ticket 42d43d501d] [Ticket c3dd68d3d3]
 */

#include "tfb_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

#define SUBTEST(fn)                                                            \
	do {                                                                   \
		printf("[TEST] %s...\n", #fn);                                 \
		if ((fn)() == 0)                                               \
			printf("[PASS] %s\n", #fn);                            \
		else {                                                         \
			printf("[FAIL] %s\n", #fn);                            \
			g_failures++;                                          \
		}                                                              \
	} while (0)

#define TEST_ASSERT(cond)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  assertion failed: %s (line %d)\n",  \
				#cond, __LINE__);                              \
			return 1;                                              \
		}                                                              \
	} while (0)

/* The legacy reply must match main.c byte for byte. */
static const char legacy_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 14\r\n"
    "Connection: keep-alive\r\n"
    "Server: Unikraft-ENA-Benchmark-MC\r\n"
    "\r\n"
    "Hello, World!\n";

/* --- date rendering --- */

static int test_date_epoch(void)
{
	char out[64];
	int n = tfb_render_date(out, sizeof(out), 0);

	TEST_ASSERT(n == TFB_DATE_LEN);
	TEST_ASSERT(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
	return 0;
}

/*
 * Cross-check the calendar math against the host libc across 30
 * years. The unikernel has no gmtime, so the host is the reference.
 */
static int test_date_vs_libc(void)
{
	static const char *const wd[7] = {"Sun", "Mon", "Tue", "Wed",
					  "Thu", "Fri", "Sat"};
	static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr",
					   "May", "Jun", "Jul", "Aug",
					   "Sep", "Oct", "Nov", "Dec"};
	char out[64];
	time_t t;

	for (t = 0; t < 30 * 366 * 86400; t += 97 * 86400 + 12345) {
		struct tm gm;
		char ref[64];

		gmtime_r(&t, &gm);
		snprintf(ref, sizeof(ref),
			 "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[gm.tm_wday],
			 gm.tm_mday, mo[gm.tm_mon], gm.tm_year + 1900,
			 gm.tm_hour, gm.tm_min, gm.tm_sec);
		tfb_render_date(out, sizeof(out), (long long)t);
		if (strcmp(out, ref) != 0) {
			fprintf(stderr, "  tfb: %s\n  ref: %s (t=%lld)\n", out,
				ref, (long long)t);
			return 1;
		}
	}
	return 0;
}

static int test_date_negative_clamps(void)
{
	char out[64];

	TEST_ASSERT(tfb_render_date(out, sizeof(out), -5) == TFB_DATE_LEN);
	TEST_ASSERT(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
	return 0;
}

/* --- response composition --- */

static int test_compose_plain_exact(void)
{
	char buf[TFB_MAX_RESP];
	int n = tfb_compose_plain(buf, sizeof(buf),
				  "Thu, 01 Jan 1970 00:00:00 GMT");
	static const char want[] = "HTTP/1.1 200 OK\r\n"
				   "Server: unikraft-httpreply\r\n"
				   "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
				   "Content-Type: text/plain\r\n"
				   "Content-Length: 13\r\n"
				   "\r\n"
				   "Hello, World!";

	TEST_ASSERT(n == (int)sizeof(want) - 1);
	TEST_ASSERT(n > 0 && memcmp(buf, want, (size_t)n) == 0);
	/* The body has no trailing newline. */
	TEST_ASSERT(buf[n - 1] == '!');
	return 0;
}

static int test_compose_json_exact(void)
{
	char buf[TFB_MAX_RESP];
	int n =
	    tfb_compose_json(buf, sizeof(buf), "Thu, 01 Jan 1970 00:00:00 GMT");
	static const char want[] = "HTTP/1.1 200 OK\r\n"
				   "Server: unikraft-httpreply\r\n"
				   "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
				   "Content-Type: application/json\r\n"
				   "Content-Length: 27\r\n"
				   "\r\n"
				   "{\"message\":\"Hello, World!\"}";

	TEST_ASSERT(n == (int)sizeof(want) - 1);
	TEST_ASSERT(n > 0 && memcmp(buf, want, (size_t)n) == 0);
	return 0;
}

static int test_compose_404_exact(void)
{
	char buf[TFB_MAX_RESP];
	int n =
	    tfb_compose_404(buf, sizeof(buf), "Thu, 01 Jan 1970 00:00:00 GMT");

	TEST_ASSERT(n > 0);
	TEST_ASSERT(strstr(buf, "404 Not Found\r\n") != NULL);
	TEST_ASSERT(strstr(buf, "Content-Length: 9\r\n") != NULL);
	TEST_ASSERT(n >= 9 && memcmp(buf + n - 9, "Not Found", 9) == 0);
	return 0;
}

/* --- request parsing --- */

static int test_parse_simple(void)
{
	static const char req[] = "GET /plaintext HTTP/1.1\r\n"
				  "Host: x\r\n\r\n";
	struct tfb_request r;
	int n = tfb_parse_request(req, strlen(req), &r);

	TEST_ASSERT(n == (int)strlen(req));
	TEST_ASSERT(r.route == TFB_ROUTE_PLAIN);
	TEST_ASSERT(r.keep_alive == 1);
	return 0;
}

static int test_parse_routes(void)
{
	static const struct {
		const char *req;
		enum tfb_route route;
	} cases[] = {
	    {"GET / HTTP/1.1\r\n\r\n", TFB_ROUTE_LEGACY},
	    {"GET /json HTTP/1.1\r\n\r\n", TFB_ROUTE_JSON},
	    {"GET /plaintext HTTP/1.1\r\n\r\n", TFB_ROUTE_PLAIN},
	    {"GET /plaintext?q=1 HTTP/1.1\r\n\r\n", TFB_ROUTE_PLAIN},
	    {"GET /other HTTP/1.1\r\n\r\n", TFB_ROUTE_NOT_FOUND},
	    {"GET /plaintextx HTTP/1.1\r\n\r\n", TFB_ROUTE_NOT_FOUND},
	    {"HEAD /json HTTP/1.1\r\n\r\n", TFB_ROUTE_JSON},
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct tfb_request r;
		int n =
		    tfb_parse_request(cases[i].req, strlen(cases[i].req), &r);

		TEST_ASSERT(n == (int)strlen(cases[i].req));
		TEST_ASSERT(r.route == cases[i].route);
	}
	return 0;
}

static int test_parse_close(void)
{
	static const char *const variants[] = {
	    "GET /json HTTP/1.1\r\nConnection: close\r\n\r\n",
	    "GET /json HTTP/1.1\r\nconnection: Close\r\n\r\n",
	    "GET /json HTTP/1.1\r\nCONNECTION: CLOSE\r\n\r\n",
	};
	size_t i;

	for (i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
		struct tfb_request r;
		int n = tfb_parse_request(variants[i], strlen(variants[i]), &r);

		TEST_ASSERT(n == (int)strlen(variants[i]));
		TEST_ASSERT(r.keep_alive == 0);
	}

	/* keep-alive on the reverse path stays open. */
	{
		struct tfb_request r;
		static const char req[] = "GET /json HTTP/1.1\r\n"
					  "Connection: keep-alive\r\n\r\n";
		int n = tfb_parse_request(req, strlen(req), &r);

		TEST_ASSERT(n == (int)strlen(req));
		TEST_ASSERT(r.keep_alive == 1);
	}
	return 0;
}

static int test_parse_incomplete(void)
{
	static const char part[] = "GET /json HTTP/1.1\r\nHost: x\r\n";
	struct tfb_request r;

	TEST_ASSERT(tfb_parse_request(part, strlen(part), &r) == 0);
	TEST_ASSERT(tfb_parse_request("", 0, &r) == 0);
	return 0;
}

static int test_parse_malformed(void)
{
	struct tfb_request r;
	static const char bad1[] = "GET\r\n\r\n";
	static const char bad2[] = "GET /json\r\n\r\n";
	static const char empty_method[] = " /json HTTP/1.1\r\n\r\n";
	char big[TFB_MAX_HEAD + 16];
	size_t i;

	/* No request target, no version, and no method. */
	TEST_ASSERT(tfb_parse_request(bad1, strlen(bad1), &r) == -1);
	TEST_ASSERT(tfb_parse_request(bad2, strlen(bad2), &r) == -1);
	TEST_ASSERT(tfb_parse_request(empty_method, strlen(empty_method), &r) ==
		    -1);

	/* Header block beyond the cap with no blank line. */
	memcpy(big, "GET /json HTTP/1.1\r\n", 18);
	for (i = 18; i < sizeof(big); i++)
		big[i] = (i % 2) ? 'x' : ' ';
	TEST_ASSERT(tfb_parse_request(big, sizeof(big), &r) == -1);
	return 0;
}

/*
 * Feed one pipelined stream through the same split-read loop the
 * app uses: carry the tail, parse every whole request, and check
 * the route order.
 */
static int test_parse_pipelined_split(void)
{
	static const char one[] = "GET /plaintext HTTP/1.1\r\n"
				  "Host: x\r\n\r\n";
	char stream[1024];
	char carrybuf[128];
	char work[192];
	size_t slen = 0, consumed = 0, carry = 0;
	int remaining = 10;

	while (remaining) {
		memcpy(stream + slen, one, sizeof(one) - 1);
		slen += sizeof(one) - 1;
		remaining--;
	}
	remaining = 10;

	/* Feed 192 bytes at a time, like coalesced TCP segments. */
	while (consumed < slen || carry) {
		size_t len = carry, off = 0, take;

		memcpy(work, carrybuf, carry);
		take = slen - consumed;
		if (take > sizeof(work) - carry)
			take = sizeof(work) - carry;
		memcpy(work + len, stream + consumed, take);
		consumed += take;
		len += take;

		for (;;) {
			struct tfb_request r;
			int n = tfb_parse_request(work + off, len - off, &r);

			if (n == 0)
				break;
			if (n < 0 || r.route != TFB_ROUTE_PLAIN) {
				fprintf(stderr, "  split parse failed\n");
				return 1;
			}
			off += (size_t)n;
			remaining--;
		}

		carry = len - off;
		if (carry > sizeof(carrybuf)) {
			fprintf(stderr, "  split carry overflow\n");
			return 1;
		}
		memcpy(carrybuf, work + off, carry);
	}

	TEST_ASSERT(remaining == 0);
	return 0;
}

/*
 * Edge cases for the ticket-4 validator additions: HTTP/1.0 parse,
 * Connection: close with version 1.0, and a header split exactly at
 * the segment boundary. [Ticket 2212657476]
 */
static int test_parse_http10(void)
{
	static const char onezero[] = "GET /plaintext HTTP/1.0\r\n"
				      "Host: x\r\n\r\n";
	static const char onezero_close[] = "GET /json HTTP/1.0\r\n"
					    "Connection: close\r\n\r\n";
	struct tfb_request r;
	int n;

	n = tfb_parse_request(onezero, strlen(onezero), &r);
	TEST_ASSERT(n == (int)strlen(onezero));
	TEST_ASSERT(r.route == TFB_ROUTE_PLAIN);
	/* Same rule as 1.1: open unless the request says close. The
	 * app answers HTTP/1.1, which RFC 7230 allows. */
	TEST_ASSERT(r.keep_alive == 1);

	n = tfb_parse_request(onezero_close, strlen(onezero_close), &r);
	TEST_ASSERT(n == (int)strlen(onezero_close));
	TEST_ASSERT(r.route == TFB_ROUTE_JSON);
	TEST_ASSERT(r.keep_alive == 0);
	return 0;
}

/* A request split between the request line and the headers must
 * parse in two passes, like the two TCP segments the validator
 * sends. */
static int test_parse_two_segments(void)
{
	static const char seg1[] = "GET /plaintext HTTP/1.1\r\n";
	static const char seg2[] = "Host: v\r\n\r\n";
	char work[128];
	struct tfb_request r;
	size_t len;
	int n;

	/* Segment 1 alone: incomplete, no bytes consumed. */
	TEST_ASSERT(tfb_parse_request(seg1, strlen(seg1), &r) == 0);

	/* Segment 2 arrives: the whole request parses at once. */
	memcpy(work, seg1, strlen(seg1));
	len = strlen(seg1);
	memcpy(work + len, seg2, strlen(seg2));
	len += strlen(seg2);

	n = tfb_parse_request(work, len, &r);
	TEST_ASSERT(n == (int)len);
	TEST_ASSERT(r.route == TFB_ROUTE_PLAIN);
	TEST_ASSERT(r.keep_alive == 1);
	return 0;
}

/* --- local server mode --- */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

#define SRV_MAX_CONNS 256
#define SRV_CARRY 8192
#define SRV_READBUF 8192

struct srv_conn {
	int fd;
	char carry[SRV_CARRY];
	size_t carry_len;
	int in_use;
};

/* Compose the response for one request and send it in full. On
 * EAGAIN the loop waits for POLLOUT so pipelined bursts keep their
 * order. */
static int srv_respond(int fd, const struct tfb_request *req)
{
	char buf[TFB_MAX_RESP];
	char date[TFB_DATE_LEN + 1];
	const char *body;
	size_t blen;
	size_t off = 0;

	tfb_render_date(date, sizeof(date), (long long)time(NULL));

	switch (req->route) {
	case TFB_ROUTE_PLAIN:
		blen = (size_t)tfb_compose_plain(buf, sizeof(buf), date);
		body = buf;
		break;
	case TFB_ROUTE_JSON:
		blen = (size_t)tfb_compose_json(buf, sizeof(buf), date);
		body = buf;
		break;
	case TFB_ROUTE_LEGACY:
		body = legacy_response;
		blen = sizeof(legacy_response) - 1;
		break;
	default:
		blen = (size_t)tfb_compose_404(buf, sizeof(buf), date);
		body = buf;
		break;
	}

	while (off < blen) {
		ssize_t n = send(fd, body + off, blen - off, MSG_NOSIGNAL);

		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd w;

			w.fd = fd;
			w.events = POLLOUT;
			w.revents = 0;
			if (poll(&w, 1, 5000) <= 0)
				return -1;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

/* One read + parse pass. Mirrors the app: whole requests get a
 * response each, and the partial tail is carried. Returns -1 to
 * close. */
static int srv_service(struct srv_conn *c, char *buf, size_t bufsize)
{
	size_t len = c->carry_len, off = 0;
	int wanted_close = 0;

	memcpy(buf, c->carry, c->carry_len);

again: {
	ssize_t r;

	r = recv(c->fd, buf + len, bufsize - 1 - len, 0);
	if (r <= 0) {
		if (r == 0)
			return wanted_close ? -1 : (len ? 0 : -1);
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (wanted_close)
				return -1;
			c->carry_len = len - off;
			if (c->carry_len > SRV_CARRY)
				return -1;
			memcpy(c->carry, buf + off, c->carry_len);
			return 0;
		}
		return -1;
	}
	len += (size_t)r;
}

	for (;;) {
		struct tfb_request req;
		int n = tfb_parse_request(buf + off, len - off, &req);

		if (n == 0)
			break;
		if (n < 0)
			return -1;
		off += (size_t)n;
		if (srv_respond(c->fd, &req) < 0)
			return -1;
		if (!req.keep_alive)
			wanted_close = 1;
	}

	if (wanted_close)
		return -1;

	c->carry_len = len - off;
	if (c->carry_len > SRV_CARRY)
		return -1;
	memcpy(c->carry, buf + off, c->carry_len);

	/* Pull more if the peer pipelined beyond our buffer. */
	if (len == bufsize - 1)
		goto again;
	return 0;
}

static int serve(unsigned short port)
{
	struct sockaddr_in addr;
	struct srv_conn conns[SRV_MAX_CONNS];
	struct pollfd pfds[SRV_MAX_CONNS + 1];
	char rbuf[SRV_READBUF];
	struct sigaction sa;
	int lf, i, nfds = 1;

	memset(conns, 0, sizeof(conns));

	lf = socket(AF_INET, SOCK_STREAM, 0);
	if (lf < 0) {
		perror("socket");
		return 1;
	}
	{
		int one = 1;

		setsockopt(lf, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (bind(lf, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	if (listen(lf, 64) < 0) {
		perror("listen");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	pfds[0].fd = lf;
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;

	printf("tfb_host_server: listening on 127.0.0.1:%u\n", port);
	fflush(stdout);

	while (!g_stop) {
		int n = poll(pfds, (nfds_t)nfds, 500);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 1;
		}
		if (n == 0)
			continue;

		if (pfds[0].revents & POLLIN) {
			int cfd = accept(lf, NULL, NULL);

			if (cfd >= 0) {
				fcntl(cfd, F_SETFL, O_NONBLOCK);
				for (i = 1; i <= SRV_MAX_CONNS; i++) {
					if (!conns[i - 1].in_use) {
						conns[i - 1].fd = cfd;
						conns[i - 1].in_use = 1;
						conns[i - 1].carry_len = 0;
						pfds[i].fd = cfd;
						pfds[i].events = POLLIN;
						pfds[i].revents = 0;
						if (i >= nfds)
							nfds = i + 1;
						break;
					}
				}
				if (i > SRV_MAX_CONNS)
					close(cfd);
			}
		}

		for (i = 1; i < nfds; i++) {
			if (pfds[i].fd < 0 || pfds[i].revents == 0)
				continue;
			if (srv_service(&conns[i - 1], rbuf, sizeof(rbuf)) <
			    0) {
				close(pfds[i].fd);
				conns[i - 1].in_use = 0;
				conns[i - 1].carry_len = 0;
				pfds[i].fd = -1;
			}
		}

		/* Compact the poll-set tail. */
		while (nfds > 1 && pfds[nfds - 1].fd < 0)
			nfds--;
	}

	for (i = 0; i < nfds; i++)
		if (pfds[i].fd >= 0)
			close(pfds[i].fd);
	close(lf);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "--serve") == 0) {
		unsigned long p = strtoul(argv[2], NULL, 10);

		if (p == 0 || p > 65535) {
			fprintf(stderr, "bad port: %s\n", argv[2]);
			return 2;
		}
		return serve((unsigned short)p);
	}

	SUBTEST(test_date_epoch);
	SUBTEST(test_date_vs_libc);
	SUBTEST(test_date_negative_clamps);
	SUBTEST(test_compose_plain_exact);
	SUBTEST(test_compose_json_exact);
	SUBTEST(test_compose_404_exact);
	SUBTEST(test_parse_simple);
	SUBTEST(test_parse_routes);
	SUBTEST(test_parse_close);
	SUBTEST(test_parse_incomplete);
	SUBTEST(test_parse_malformed);
	SUBTEST(test_parse_pipelined_split);
	SUBTEST(test_parse_http10);
	SUBTEST(test_parse_two_segments);

	if (g_failures) {
		printf("test_tfb_http: %d test(s) FAILED\n", g_failures);
		return 1;
	}
	printf("test_tfb_http: all tests passed\n");
	return 0;
}