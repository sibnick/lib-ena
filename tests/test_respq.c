/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 *
 * Host tests for the bounded response queue and fixed node pool in
 * samples/httpreply-mc/tfb_respq.h. [Ticket 0f377016a9]
 *
 * Unit tests check pool reuse, the per-connection cap, push order,
 * and drain accounting. The integration test wires tfb_http.h and
 * tfb_respq.h to a small-buffer socketpair. A server thread runs the
 * same parse/queue/send pass the app runs. A client pipelines a
 * request burst deeper than the cap, then reads every response. The
 * test asserts the queue stayed under the cap while the client was
 * not reading, every response arrived in order, and the connection
 * resumed after the queue drained.
 */

#include "tfb_http.h"
#include "tfb_respq.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
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

#define PLAIN_BODY "Hello, World!"
#define JSON_BODY "{\"message\":\"Hello, World!\"}"
#define BODY_SLOT 40

/* --- pool --- */

static int test_pool_reuse(void)
{
	struct tfb_resp slots[8];
	struct tfb_resp_pool p;
	struct tfb_resp *r;
	int i;

	tfb_pool_init(&p, slots, 8);
	TEST_ASSERT(p.free_count == 8);

	for (i = 0; i < 8; i++) {
		r = tfb_pool_get(&p);
		TEST_ASSERT(r != NULL);
		/* Every node comes from the fixed array: no heap. */
		TEST_ASSERT(r >= slots && r < slots + 8);
	}
	TEST_ASSERT(tfb_pool_get(&p) == NULL);
	TEST_ASSERT(p.free_count == 0);

	tfb_pool_put(&p, &slots[3]);
	TEST_ASSERT(p.free_count == 1);
	r = tfb_pool_get(&p);
	TEST_ASSERT(r == &slots[3]);
	return 0;
}

/* --- queue cap --- */

static int test_push_cap(void)
{
	static struct tfb_resp slots[TFB_RESPQ_SLOTS];
	struct tfb_resp_pool p;
	struct tfb_respq q;
	char data[64];
	int i;

	memset(&q, 0, sizeof(q));
	tfb_pool_init(&p, slots, TFB_RESPQ_SLOTS);
	memset(data, 'x', sizeof(data));

	for (i = 0; i < TFB_RESPQ_CAP; i++)
		TEST_ASSERT(tfb_respq_push(&q, &p, data, (int)sizeof(data)) ==
			    0);
	TEST_ASSERT(q.count == TFB_RESPQ_CAP);
	TEST_ASSERT(tfb_respq_room(&q, &p) == 0);

	/* At the cap a push fails and changes nothing. */
	TEST_ASSERT(tfb_respq_push(&q, &p, data, (int)sizeof(data)) == -1);
	TEST_ASSERT(q.count == TFB_RESPQ_CAP);
	TEST_ASSERT(p.free_count == TFB_RESPQ_SLOTS - TFB_RESPQ_CAP);

	tfb_respq_reset(&q, &p);
	TEST_ASSERT(q.count == 0);
	TEST_ASSERT(q.head == NULL && q.tail == NULL);
	TEST_ASSERT(p.free_count == TFB_RESPQ_SLOTS);
	return 0;
}

/* A pool smaller than the cap must flow control on pool exhaustion
 * and reopen once sent nodes return. */
static int test_pool_exhaust_resume(void)
{
	struct tfb_resp slots[4];
	struct tfb_resp_pool p;
	struct tfb_respq q;
	int sv[2], i, rc;
	char data[32];
	char in[512];
	size_t got = 0;

	tfb_pool_init(&p, slots, 4);
	memset(&q, 0, sizeof(q));
	memset(data, 'y', sizeof(data));

	for (i = 0; i < 4; i++)
		TEST_ASSERT(tfb_respq_push(&q, &p, data, (int)sizeof(data)) ==
			    0);
	/* Pool empty: no room even though the cap is far away. */
	TEST_ASSERT(q.count == 4);
	TEST_ASSERT(tfb_respq_room(&q, &p) == 0);
	TEST_ASSERT(tfb_respq_push(&q, &p, data, (int)sizeof(data)) == -1);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	/* Send everything before reading the peer: closing the last
	 * write reference of a unix socket drops unread data, and a
	 * blocking read here would wait for a writer that is gone. */
	do {
		rc = tfb_respq_send(&q, &p, sv[0], NULL, NULL);
	} while (rc == 1);
	TEST_ASSERT(rc == 0);
	TEST_ASSERT(q.count == 0);
	TEST_ASSERT(p.free_count == 4);
	/* Room is back: the connection can queue again. */
	TEST_ASSERT(tfb_respq_room(&q, &p) == 1);
	TEST_ASSERT(tfb_respq_push(&q, &p, data, (int)sizeof(data)) == 0);
	/* The buffers still hold the first batch: resume-send with
	 * MSG_DONTWAIT so a full buffer cannot block this thread. */
	do {
		rc = send(sv[0], data, sizeof(data), MSG_DONTWAIT);
	} while (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
	TEST_ASSERT(rc == (ssize_t)sizeof(data));
	close(sv[0]);

	while (got < sizeof(data) * 5) {
		ssize_t n = read(sv[1], in + got, sizeof(in) - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(sv[1]);
	/* Four drained batches plus the resume batch all arrive. */
	TEST_ASSERT(got == sizeof(data) * 5);
	for (i = 0; i < 5; i++)
		TEST_ASSERT(memcmp(in + i * (int)sizeof(data), data,
				   sizeof(data)) == 0);
	return 0;
}

/* --- send order --- */

static int test_send_order(void)
{
	struct tfb_resp slots[3];
	struct tfb_resp_pool p;
	struct tfb_respq q;
	int sv[2], rc;
	char in[512];
	char expect[16];
	size_t elen = 0, got = 0;
	const char *want[3] = {"AAAA", "BB", "C"};
	int i;

	tfb_pool_init(&p, slots, 3);
	memset(&q, 0, sizeof(q));
	for (i = 0; i < 3; i++) {
		size_t l = strlen(want[i]);
		memcpy(expect + elen, want[i], l);
		elen += l;
		TEST_ASSERT(tfb_respq_push(&q, &p, want[i], (int)l) == 0);
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	do {
		rc = tfb_respq_send(&q, &p, sv[0], NULL, NULL);
	} while (rc == 1);
	close(sv[0]);
	TEST_ASSERT(rc == 0);
	TEST_ASSERT(p.free_count == 3);

	while (got < elen) {
		ssize_t n = read(sv[1], in + got, elen - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(sv[1]);
	TEST_ASSERT(got == elen);
	TEST_ASSERT(memcmp(in, expect, elen) == 0);
	return 0;
}

/*
 * Deep-pipeline integration test. One thread runs the server, one
 * thread the client. The server pass below mirrors the app's
 * handle_readable() and send loop: drain the queue, read when the
 * queue has room, parse whole requests while room lasts, queue one
 * composed response each, and carry the unparsed tail. The legacy
 * static fast path is app-only and not modeled here.
 */
#define SIM_CARRY 4096
#define SIM_READBUF 4096
#define N_REQ 800 /* deeper than TFB_RESPQ_CAP */
#define IN_BUFSZ 65536

struct sim_conn {
	int fd;
	struct tfb_respq q;
	char carry[SIM_CARRY];
	uint16_t carry_len;
	int parsed;
};

/* Server-thread state. Only the server thread writes parsed and
 * max_q; the client reads them after the phase barriers. */
struct sim_srv {
	int fd;
	volatile int stop;
	int parsed;
	int max_q;
	struct tfb_respq q;
	char carry[SIM_CARRY];
	uint16_t carry_len;
};

static int sim_pass(struct sim_srv *s, struct tfb_resp_pool *p)
{
	char buf[SIM_READBUF];
	char tmp[TFB_MAX_RESP];
	char date[TFB_DATE_LEN + 1];
	size_t len = 0, off = 0;
	ssize_t r;
	int rc;

	/* Drain first: a full queue must still send. */
	rc = tfb_respq_send(&s->q, p, s->fd, NULL, NULL);
	if (rc < 0)
		return -1;
	if (s->q.count > (unsigned int)s->max_q)
		s->max_q = (int)s->q.count;

	/* Read only while the queue has room. With a full queue the
	 * requests wait in the socket buffer: that is the flow
	 * control the ticket asks for. */
	if (rc != 1 && tfb_respq_room(&s->q, p)) {
		if (s->carry_len) {
			memcpy(buf, s->carry, s->carry_len);
			len = s->carry_len;
			s->carry_len = 0;
		}

		/* A full carry already fills the read buffer. */
		if (len >= sizeof(buf) - 1) {
			r = -1;
			errno = EAGAIN;
		} else {
			r = recv(s->fd, buf + len, sizeof(buf) - 1 - len, 0);
		}
		if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (r > 0)
			len += (size_t)r;
		else if (r == 0)
			return -1; /* client closed */
		else if (len) {
			/* No new bytes: put the carry back and let
			 * this pass parse what we already hold. */
		}

		tfb_render_date(date, sizeof(date), (long long)time(NULL));

		for (;;) {
			struct tfb_request req;
			int consumed, blen;

			if (!tfb_respq_room(&s->q, p))
				break;
			consumed =
			    tfb_parse_request(buf + off, len - off, &req);
			if (consumed == 0)
				break;
			if (consumed < 0)
				return -1;
			off += (size_t)consumed;
			s->parsed++;

			switch (req.route) {
			case TFB_ROUTE_JSON:
				blen = tfb_compose_json(tmp, sizeof(tmp), date);
				break;
			case TFB_ROUTE_PLAIN:
				blen =
				    tfb_compose_plain(tmp, sizeof(tmp), date);
				break;
			default:
				blen = tfb_compose_404(tmp, sizeof(tmp), date);
				break;
			}
			if (blen < 0)
				return -1;
			if (tfb_respq_push(&s->q, p, tmp, blen) != 0)
				return -1;
			rc = tfb_respq_send(&s->q, p, s->fd, NULL, NULL);
			if (rc < 0)
				return -1;
			if (rc == 1)
				break; /* socket full: stop parsing */
			if (s->q.count > (unsigned int)s->max_q)
				s->max_q = (int)s->q.count;
		}

		if (off < len) {
			size_t tail = len - off;
			if (tail > SIM_CARRY)
				return -1;
			memcpy(s->carry, buf + off, tail);
			s->carry_len = (uint16_t)tail;
		}
	}
	return 0;
}

static struct sim_srv g_srv;
static struct tfb_resp g_slots[TFB_RESPQ_SLOTS];

static void *srv_thread(void *arg)
{
	struct tfb_resp_pool p;
	struct sim_srv *s = arg;

	tfb_pool_init(&p, g_slots, TFB_RESPQ_SLOTS);
	while (!s->stop)
		if (sim_pass(s, &p) < 0)
			break;
	return NULL;
}

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);

	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Find buf[0..len-1] case-insensitively. Return the index or -1. */
static long find_ci(const char *buf, size_t len, const char *needle)
{
	size_t n = strlen(needle), i, j;

	for (i = 0; i + n <= len; i++) {
		for (j = 0; j < n; j++)
			if (!tfb_ci_eq(buf[i + j], needle[j]))
				break;
		if (j == n)
			return (long)i;
	}
	return -1;
}

/* Sleep about 1 ms without leaving the C99 + POSIX.1-2008 feature
 * set the harness compiles with. */
static void nap_ms(void)
{
	struct timespec ts = {0, 1000000};

	nanosleep(&ts, NULL);
}

/* Pull complete responses from the non-blocking client fd. Copy each
 * 200 body into out_bodies[nbody] when there is room. Return -1 on a
 * malformed response, a read fault, or timeout_ms elapsing with no
 * progress; 0 when no full response is pending. */
static int client_collect(int fd, char *inbuf, size_t incap, size_t *inlen,
			  char out_bodies[][BODY_SLOT], int maxb, int *nbody,
			  unsigned int timeout_ms)
{
	struct timespec t0, t;
	long base = *nbody;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		size_t i = 0, hdr, clen, need;
		long cl;
		char *body;
		int found = 0;

		while (i + 4 <= *inlen) {
			if (inbuf[i] == '\r' && inbuf[i + 1] == '\n' &&
			    inbuf[i + 2] == '\r' && inbuf[i + 3] == '\n') {
				found = 1;
				break;
			}
			i++;
		}
		if (found) {
			hdr = i;
			cl = find_ci(inbuf, hdr, "content-length:");
			if (cl < 0)
				return -1;
			clen = (size_t)strtoul(inbuf + cl + 15, NULL, 10);
			need = hdr + 4 + clen;
			if (*inlen >= need) {
				body = inbuf + hdr + 4;
				if (*inlen < 12 ||
				    memcmp(inbuf, "HTTP/1.1 200", 12) != 0)
					return -1;
				if (clen >= BODY_SLOT)
					return -1;
				if (*nbody < maxb) {
					memcpy(out_bodies[*nbody], body, clen);
					out_bodies[*nbody][clen] = '\0';
				}
				(*nbody)++;
				memmove(inbuf, body + clen, *inlen - need);
				*inlen -= need;
				continue;
			}
		}
		{
			ssize_t n =
			    read(fd, inbuf + *inlen, incap - 1 - *inlen);
			if (n > 0) {
				*inlen += (size_t)n;
				continue;
			}
			if (n < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK)) {
				if (*nbody != base)
					return 0; /* progress */
				clock_gettime(CLOCK_MONOTONIC, &t);
				if ((t.tv_sec - t0.tv_sec) * 1000 +
					(t.tv_nsec - t0.tv_nsec) / 1000000 >=
				    (long)timeout_ms)
					return -1; /* stalled */
				nap_ms();
				continue;
			}
			return -1;
		}
	}
}

/* Write buf to the client fd for at most timeout_ms. A full socket
 * makes this return early: that backpressure is the point. Return 0
 * on a complete write, 1 on a partial write that timed out, -1 on a
 * fault. */
/* Push as much of buf as fits while the socket accepts, for at most
 * timeout_ms. A full socket makes this return early with the byte
 * count so far: that backpressure is the point. Return the count
 * (possibly < len), or -1 on a fault. */
static ssize_t client_write_burst(int fd, const char *buf, size_t len,
				  unsigned int timeout_ms)
{
	struct timespec t0, t;
	size_t off = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN ||
			      errno == EWOULDBLOCK)) {
			clock_gettime(CLOCK_MONOTONIC, &t);
			if ((t.tv_sec - t0.tv_sec) * 1000 +
				(t.tv_nsec - t0.tv_nsec) / 1000000 >=
			    (long)timeout_ms)
				return (ssize_t)off;
			nap_ms();
			continue;
		}
		return -1;
	}
	return (ssize_t)off;
}

/* Write as much of buf as the non-blocking fd accepts right now.
 * Return the byte count written (possibly partial, < len), or -1 on
 * a fault. A partial write is the socket backpressure the test
 * relies on. */
static ssize_t client_write_now(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 &&
		    (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		return -1;
	}
	return (ssize_t)off;
}

static int test_deep_pipeline(void)
{
	pthread_t th;
	int sv[2] = {-1, -1};
	static char got_bodies[N_REQ][BODY_SLOT];
	char *out, inbuf[IN_BUFSZ];
	size_t inlen = 0, wtotal = 0, woff = 0;
	int i, nbody = 0;
	int ok = 1, rc = 1;
	struct timespec t0, t;

	out = malloc(65536);
	if (!out)
		return 1;

	/* Alternate routes so an out-of-order response is visible. */
	for (i = 0; i < N_REQ; i++) {
		const char *uri = (i % 2 == 0) ? "/plaintext" : "/json";
		int n = snprintf(out + wtotal, 65536 - wtotal,
				 "GET %s HTTP/1.1\r\nHost: t\r\n\r\n", uri);
		if (n < 0)
			goto done;
		wtotal += (size_t)n;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		goto done;
	/* Small buffers: the client cannot park the whole burst in
	 * the socket, so the queue cap must engage. */
	{
		int sz = 8192;
		setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	}
	if (set_nonblock(sv[0]) || set_nonblock(sv[1]))
		goto done;

	memset(&g_srv, 0, sizeof(g_srv));
	g_srv.fd = sv[0];
	if (pthread_create(&th, NULL, srv_thread, &g_srv) != 0)
		goto done;

	/* Phase 1: the client pushes the burst while it still fits.
	 * The socket backpressure stalls the writer while the server
	 * queue sits at the cap, so the server cannot parse the whole
	 * burst with no reader. */
	{
		ssize_t wr = client_write_burst(sv[1], out, wtotal, 300);

		if (wr < 0)
			goto join;
		if (g_srv.parsed >= N_REQ) {
			fprintf(stderr,
				"  flow control did not engage: server "
				"parsed %d of %d with no client reads\n",
				g_srv.parsed, N_REQ);
			goto join;
		}
		if (g_srv.max_q > TFB_RESPQ_CAP) {
			fprintf(stderr, "  queue exceeded the cap: %d\n",
				g_srv.max_q);
			goto join;
		}
		printf("  flow control held the server at %d parsed, queue "
		       "max %d\n",
		       g_srv.parsed, g_srv.max_q);
		woff = (size_t)wr;
	}

	/* Phase 2: the client finishes the burst and reads at the
	 * same time, the way a real pipelining client does. Writing
	 * while reading keeps the socket moving in both directions.
	 * Every response must arrive, in order. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (nbody < N_REQ) {
		if (woff < wtotal) {
			ssize_t n =
			    client_write_now(sv[1], out + woff, wtotal - woff);

			if (n < 0)
				goto join;
			woff += (size_t)n;
		}
		if (client_collect(sv[1], inbuf, IN_BUFSZ, &inlen, got_bodies,
				   N_REQ, &nbody, 5000) < 0) {
			fprintf(stderr,
				"  stalled at %d of %d responses (parsed=%d "
				"queued=%u woff=%zu/%zu)\n",
				nbody, N_REQ, g_srv.parsed,
				(unsigned)g_srv.q.count, woff, wtotal);
			goto join;
		}
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) > 30) {
			fprintf(stderr, "  timed out at %d of %d\n", nbody,
				N_REQ);
			goto join;
		}
	}
	if (g_srv.max_q > TFB_RESPQ_CAP) {
		fprintf(stderr, "  queue exceeded the cap: %d\n", g_srv.max_q);
		goto join;
	}

	for (i = 0; i < N_REQ; i++) {
		const char *want =
		    (i % 2 == 0) ? PLAIN_BODY ": 13" : JSON_BODY ": 27";
		char gotc[BODY_SLOT + 8];
		snprintf(gotc, sizeof(gotc), "%.39s: %zu", got_bodies[i],
			 strlen(got_bodies[i]));
		if (strcmp(gotc, want) != 0) {
			fprintf(stderr, "  response %d: got %s want %s\n", i,
				gotc, want);
			ok = 0;
			break;
		}
	}
	if (!ok)
		goto join;

	/* Phase 3: the drained connection resumes. */
	{
		static const char one[] =
		    "GET /plaintext HTTP/1.1\r\nHost: t\r\n\r\n";
		size_t poff = 0;
		int got5 = 0;

		/* Write the five bytes-equivalent requests while
		 * reading: the same interleaved pattern. */
		clock_gettime(CLOCK_MONOTONIC, &t0);
		while (got5 < 5) {
			size_t pend = 5 * (sizeof(one) - 1);
			char fivebuf[5 * (sizeof(one) - 1)];
			int k;

			for (k = 0; k < 5; k++)
				memcpy(fivebuf + k * (sizeof(one) - 1), one,
				       sizeof(one) - 1);
			if (poff < pend) {
				ssize_t n = client_write_now(
				    sv[1], fivebuf + poff, pend - poff);

				if (n < 0)
					goto join;
				poff += (size_t)n;
			}
			{
				int nb0 = nbody;

				if (client_collect(sv[1], inbuf, IN_BUFSZ,
						   &inlen, NULL, 0, &nbody,
						   5000) < 0)
					goto join;
				got5 += nbody - nb0;
			}
			if (got5 > 5) {
				fprintf(stderr, "  extra responses: %d\n",
					got5);
				goto join;
			}
			clock_gettime(CLOCK_MONOTONIC, &t);
			if ((t.tv_sec - t0.tv_sec) > 10)
				break;
		}
		if (got5 != 5) {
			fprintf(stderr,
				"  connection did not resume: %d of 5\n", got5);
			goto join;
		}
		printf("  drained connection resumed with 5 more responses\n");
	}
	rc = 0;
join:
	g_srv.stop = 1;
	pthread_join(th, NULL);
done:
	if (sv[0] >= 0)
		close(sv[0]);
	if (sv[1] >= 0)
		close(sv[1]);
	free(out);
	return rc;
}

int main(void)
{
	SUBTEST(test_pool_reuse);
	SUBTEST(test_push_cap);
	SUBTEST(test_pool_exhaust_resume);
	SUBTEST(test_send_order);
	SUBTEST(test_deep_pipeline);

	if (g_failures) {
		printf("test_respq: %d test(s) FAILED\n", g_failures);
		return 1;
	}
	printf("test_respq: all tests passed\n");
	return 0;
}