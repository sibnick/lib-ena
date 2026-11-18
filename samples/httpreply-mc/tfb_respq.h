/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bounded per-connection response queue with a fixed node pool.
 *
 * The app queues one composed response per routed request, in
 * request order, until the socket accepts them. Two rules keep the
 * per-core heap bounded and the hot path free of malloc:
 * [Ticket 0f377016a9]
 * 1. Each connection holds at most TFB_RESPQ_CAP queued responses.
 *    When the queue is full the caller stops parsing and stops
 *    reading from the socket. The unread requests then wait in the
 *    TCP window. Nothing is dropped: the queued responses stay in
 *    order and complete.
 * 2. Response nodes come from a per-worker pool of fixed slots,
 *    allocated once at worker start. Queueing copies the composed
 *    bytes into a slot and never calls malloc. After a node is sent
 *    it returns to the pool. When the pool runs empty the same flow
 *    control applies: push fails, the caller keeps the request, and
 *    the connection resumes once other connections drain.
 *
 * The code is plain C with no unikraft dependency, so the host test
 * harness (tests/test_respq.c) runs the same queue the app uses.
 */

#ifndef TFB_RESPQ_H
#define TFB_RESPQ_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "tfb_http.h"

/* Per-connection limit for queued responses. The compliance
 * validator pipelines 100 requests on one connection, so the cap
 * must be at least 100. 128 keeps that headroom in a power of two. */
#define TFB_RESPQ_CAP 128

/* Response node count per worker. Every queued response on every
 * connection of one worker uses one slot. At the cap of 128 per
 * connection, a full pool flows control on the remaining
 * connections instead of growing the heap. */
#define TFB_RESPQ_SLOTS 2048

/* One queued response. The caller composes the bytes with the
 * helpers in tfb_http.h; push copies them into data. */
struct tfb_resp {
	struct tfb_resp *next;
	uint32_t len;
	uint32_t off;
	char data[TFB_MAX_RESP];
};

/* Per-connection FIFO of response nodes. count never exceeds
 * TFB_RESPQ_CAP. */
struct tfb_respq {
	struct tfb_resp *head;
	struct tfb_resp *tail;
	uint16_t count;
};

/* Per-worker pool of fixed response slots. slots is a caller-owned
 * array of n entries; the LIFO free list reuses sent nodes. */
struct tfb_resp_pool {
	struct tfb_resp *slots;
	struct tfb_resp *free_head;
	uint32_t free_count;
};

static inline void tfb_pool_init(struct tfb_resp_pool *p,
				 struct tfb_resp *slots, uint32_t n)
{
	uint32_t i;

	p->slots = slots;
	p->free_head = NULL;
	p->free_count = n;
	for (i = 0; i < n; i++) {
		slots[i].next = p->free_head;
		p->free_head = &slots[i];
	}
}

static inline struct tfb_resp *tfb_pool_get(struct tfb_resp_pool *p)
{
	struct tfb_resp *r = p->free_head;

	if (!r)
		return NULL;
	p->free_head = r->next;
	p->free_count--;
	return r;
}

static inline void tfb_pool_put(struct tfb_resp_pool *p, struct tfb_resp *r)
{
	r->next = p->free_head;
	p->free_head = r;
	p->free_count++;
}

/* Return 1 when a push would still find room. The caller checks this
 * before parsing the next request, so a parsed request never lacks a
 * node. */
static inline int tfb_respq_room(const struct tfb_respq *q,
				 const struct tfb_resp_pool *p)
{
	return q->count < TFB_RESPQ_CAP && p->free_count > 0;
}

/* Copy one composed response into a pooled node and append it.
 * Return 0 on success, or -1 when the queue is at the cap, the pool
 * is empty, or len does not fit. On -1 nothing is queued, so the
 * caller keeps the request for the next pass. */
static inline int tfb_respq_push(struct tfb_respq *q, struct tfb_resp_pool *p,
				 const char *data, int len)
{
	struct tfb_resp *r;

	if (len <= 0 || len > TFB_MAX_RESP || q->count >= TFB_RESPQ_CAP)
		return -1;
	r = tfb_pool_get(p);
	if (!r)
		return -1;
	r->next = NULL;
	r->len = (uint32_t)len;
	r->off = 0;
	memcpy(r->data, data, (size_t)len);
	if (q->tail)
		q->tail->next = r;
	else
		q->head = r;
	q->tail = r;
	q->count++;
	return 0;
}

/* Called when send() finds the socket full. The app passes a drive
 * function that moves its own network stack; the host test passes
 * NULL. */
typedef void (*tfb_drive_fn)(void *arg);

/* Send the queued responses in queue order. A node leaves the queue
 * only after every byte was sent. Return values:
 *  0: the queue is empty
 *  1: the socket is full, the rest stays queued in order
 * -1: send failed, the caller must drop the connection
 */
static inline int tfb_respq_send(struct tfb_respq *q, struct tfb_resp_pool *p,
				 int fd, tfb_drive_fn drive, void *arg)
{
	while (q->head) {
		struct tfb_resp *r = q->head;

		while (r->off < r->len) {
			ssize_t n = send(fd, r->data + r->off,
					 (size_t)(r->len - r->off), 0);

			if (n > 0) {
				r->off += (uint32_t)n;
				continue;
			}

			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
				      errno == ENOBUFS || errno == EBUSY)) {
				if (drive)
					drive(arg);
				n = send(fd, r->data + r->off,
					 (size_t)(r->len - r->off), 0);
				if (n > 0) {
					r->off += (uint32_t)n;
					continue;
				}
				return 1;
			}

			return -1;
		}

		q->head = r->next;
		if (!q->head)
			q->tail = NULL;
		q->count--;
		tfb_pool_put(p, r);
	}
	return 0;
}

/* Return every queued node to the pool and clear the queue. */
static inline void tfb_respq_reset(struct tfb_respq *q, struct tfb_resp_pool *p)
{
	while (q->head) {
		struct tfb_resp *r = q->head;

		q->head = r->next;
		q->count--;
		tfb_pool_put(p, r);
	}
	q->tail = NULL;
	q->count = 0;
}

#endif /* TFB_RESPQ_H */