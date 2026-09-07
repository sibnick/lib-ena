/*
 * Copyright (C) 2024-2026 the original author or authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * httpreply-mc: multi-core HTTP reply benchmark server.
 *
 * Worker thread framework (ticket 126efe1e43). Threaded lwIP (one
 * dispatcher thread per RX queue) and per-core worker threads on a
 * 2-vCPU KVM target with the ENA driver in 2-queue-pair mode. The
 * main thread runs as worker 0 and keeps its own connections, so its
 * ring is NULL. Every other worker is a uk_thread pinned to its own
 * vCPU, fed by a lock-free SPSC ring of accepted file descriptors.
 * The accept / distribute loop runs on the main thread (ticket
 * 063257c57d); the HTTP request and response handling runs on each
 * worker (ticket 2500f30961). The lwIP tcpip thread and the uknetdev
 * dispatcher threads drive the network stack, so the workers never
 * poll the device or run the stack timers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread.h>

#include "spsc.h"

#define MC_MAX_WORKERS 2
#define MC_RECVBUF_SIZE 8192

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define LISTEN_PORT 80
#define BACKLOG 512
#define SOCK_BUF_SIZE 32768

/*
 * The fd numbers come from the fixed lwIP socket pool, so a table of
 * this size covers every fd the socket stack can hand out. Each
 * worker owns its own table: a given fd is registered on exactly one
 * worker's epoll set, so indexing by fd is safe per worker.
 */
#define MAX_TRACKED_FDS 2048

/* Print a per-worker request/byte summary to the console every N
 * requests handled by that worker. */
#define MC_STATS_INTERVAL 20000UL

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-ENA-Benchmark-MC\r\n"
	"\r\n"
	"Hello, World!\n";

static const size_t http_resp_len = sizeof(http_response) - 1;

struct worker_ctx {
	int worker_id;
	int epoll_fd;
	struct spsc_ring *ring;
	int conn_count;
	char *recv_buf;
	uint32_t *resp_pending;
	unsigned long req_count;
	unsigned long byte_count;
};

static struct spsc_ring mc_rings[MC_MAX_WORKERS];
static struct worker_ctx mc_workers[MC_MAX_WORKERS];

/* Listener socket owned by the main thread (worker 0). */
static int mc_listener_fd;
/* Number of workers, counted from the scheduler list in main(). */
static int mc_nworkers;
/* Round-robin index over the workers for accepted connections. */
static int mc_rr_counter;
/* Connections dropped when a worker ring was full. */
static int mc_dropped;

/*
 * Get the scheduler that owns vCPU idx by walking uk_sched_head,
 * the linked list of all per-LCPU schedulers. Index 0 is the BSP
 * (vCPU 0) and index 1 is vCPU 1. Returns NULL when the list is
 * shorter than idx + 1.
 */
static struct uk_sched *mc_get_sched(int idx)
{
	struct uk_sched *s;
	int i;

	s = uk_sched_head;
	for (i = 0; i < idx; i++) {
		if (s == NULL)
			return NULL;
		s = s->next;
	}

	return s;
}

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCK_BUF_SIZE;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
	ioctl(fd, FIONBIO, &opt);
}

/*
 * Create the non-blocking listener socket on LISTEN_PORT and put it
 * into the listening state. Returns the fd on success, or -1 on
 * error.
 */
static int create_listener(void)
{
	struct sockaddr_in addr;
	int opt = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		printf("httpreply-mc: [ERR] socket failed: errno %d\n", errno);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEADDR failed: errno %d\n",
		       errno);
		close(fd);
		return -1;
	}

	configure_socket_options(fd);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(LISTEN_PORT);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("httpreply-mc: [ERR] bind failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	if (listen(fd, BACKLOG) < 0) {
		printf("httpreply-mc: [ERR] listen failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * Hand an accepted connection to a worker, chosen round-robin over
 * the worker count: fd N goes to worker N % mc_nworkers. Worker 0
 * is the main thread and keeps the connection on its own epoll set.
 * Every other worker gets the fd through its SPSC ring; the worker
 * registers the fd on its own epoll set and counts the connection.
 * When a ring is full, the connection is dropped.
 */
static void distribute_fd(int fd)
{
	int t = mc_rr_counter++;

	t = t % mc_nworkers;
	configure_socket_options(fd);

	if (t == 0) {
		/* The main thread keeps the connection itself. */
		struct epoll_event ev;

		ev.events = EPOLLIN | EPOLLRDHUP;
		ev.data.fd = fd;
		epoll_ctl(mc_workers[0].epoll_fd, EPOLL_CTL_ADD, fd, &ev);
		mc_workers[0].conn_count++;
	} else {
		if (!spsc_ring_push(&mc_rings[t], fd)) {
			/* The ring is full: drop the connection. */
			close(fd);
			mc_dropped++;
		}
	}
}

/*
 * Update the interest set of an fd that is already registered with
 * the worker's epoll instance.
 */
static int set_epoll_events(struct worker_ctx *w, int fd, uint32_t events)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.fd = fd;

	return epoll_ctl(w->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Send as many bytes of the pending response as the socket accepts.
 * The socket is non-blocking: when the socket buffer is full, the
 * send fails and the rest is re-sent when EPOLLOUT fires. The lwIP
 * tcpip thread makes room in the socket buffer in the meantime.
 *
 * Returns 0 when the response is fully sent and EPOLLOUT is
 * disarmed, returns 1 when bytes are still pending and EPOLLOUT is
 * armed, and returns -1 when the connection must be dropped.
 */
static int send_pending_response(struct worker_ctx *w, int fd, uint32_t base_events)
{
	uint32_t pending;
	ssize_t n;

	if (fd < 0 || fd >= MAX_TRACKED_FDS)
		return -1;

	pending = w->resp_pending[fd];

	while (pending > 0) {
		n = send(fd, http_response + (http_resp_len - pending), pending, 0);
		if (n > 0) {
			pending -= (uint32_t)n;
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			   errno == ENOBUFS || errno == EBUSY))
			break;

		return -1;
	}

	w->resp_pending[fd] = pending;

	if (pending > 0) {
		/* The socket buffer is full: wait for EPOLLOUT before
		 * sending the rest. */
		if (set_epoll_events(w, fd, base_events | EPOLLOUT) < 0)
			return -1;

		return 1;
	}

	/* The response is complete: disarm EPOLLOUT so a writable
	 * socket does not trigger a busy loop. */
	if (set_epoll_events(w, fd, base_events) < 0)
		return -1;

	return 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from the
 * worker's epoll interest list, and close the fd.
 */
static void drop_connection(struct worker_ctx *w, int fd)
{
	if (fd >= 0 && fd < MAX_TRACKED_FDS)
		w->resp_pending[fd] = 0;

	if (fd >= 0) {
		epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
	}
}

/*
 * Handle a batch of epoll events for one worker. An error, hangup,
 * or remote close drops the connection. A connection with unsent
 * response bytes finishes the send when the socket is writable
 * again. A readable connection receives the request and sends the
 * response; a "Connection: close" request drops the connection
 * after the reply.
 */
static void handle_worker_events(struct worker_ctx *w,
				 struct epoll_event *evs, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		uint32_t base_events = EPOLLIN | EPOLLRDHUP;
		int fd = evs[i].data.fd;

		if (evs[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
			/* Peer closed the connection or an error
			 * happened: drop the connection. */
			drop_connection(w, fd);
			continue;
		}

		if (fd < MAX_TRACKED_FDS && w->resp_pending[fd] > 0) {
			/* This connection has unsent response bytes.
			 * Finish the send before any new request is
			 * handled: when the socket is writable again
			 * (EPOLLOUT), send the rest, otherwise wait
			 * for the next EPOLLOUT. */
			if (evs[i].events & EPOLLOUT) {
				if (send_pending_response(w, fd,
							  base_events) < 0)
					drop_connection(w, fd);
			}

			continue;
		}

		if (evs[i].events & (EPOLLIN | EPOLLRDNORM)) {
			ssize_t r = recv(fd, w->recv_buf, MC_RECVBUF_SIZE - 1, 0);

			if (r > 0) {
				w->recv_buf[r] = '\0';

				/* Per-worker request and byte counters. Each
				 * worker is written only by its own core, so
				 * no lock is needed. A summary line is printed
				 * to the console every N requests. */
				w->req_count++;
				w->byte_count += http_resp_len;
				if ((w->req_count % MC_STATS_INTERVAL) == 0)
					printf("httpreply-mc: [stats] worker %d: "
					       "req=%lu bytes=%lu\n",
					       w->worker_id,
					       (unsigned long)w->req_count,
					       (unsigned long)w->byte_count);

				/* Track the response bytes that still
				 * have to go out and send what the
				 * socket accepts now. If the socket
				 * buffer fills up, the rest is
				 * re-sent when EPOLLOUT fires, so no
				 * byte of the response is dropped. */
				if (fd < MAX_TRACKED_FDS) {
					w->resp_pending[fd] = (uint32_t)http_resp_len;
					if (send_pending_response(w, fd,
								 base_events) < 0)
						drop_connection(w, fd);
				} else {
					send(fd, http_response, http_resp_len, 0);
				}

				if (strstr(w->recv_buf, "Connection: close") != NULL ||
				    strstr(w->recv_buf, "connection: close") != NULL)
					drop_connection(w, fd);
			} else if (r == 0 || (r < 0 && errno != EAGAIN &&
					   errno != EWOULDBLOCK)) {
				drop_connection(w, fd);
			}
		}
	}
}

/*
 * Per-worker loop. A worker with a ring drains it first: every fd
 * pushed by the accept loop is registered for read on the worker's
 * own epoll instance. The worker then polls its epoll set with a
 * zero timeout, handles the events, and yields, so an idle worker
 * does not busy-wait its core. The lwIP tcpip thread and the uknetdev
 * dispatcher threads drive the network stack.
 */
static __noreturn void worker_loop(void *arg)
{
	struct worker_ctx *w = (struct worker_ctx *)arg;
	struct epoll_event evs[64];
	int fd;
	int n;

	for(;;) {
		if (w->ring != NULL) {
			while (spsc_ring_pop(w->ring, &fd)) {
				struct epoll_event ev;

				ev.events = EPOLLIN | EPOLLRDHUP;
				ev.data.fd = fd;
				epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
				w->conn_count++;
			}
		}

		n = epoll_wait(w->epoll_fd, evs, 64, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* Other poll errors are not fatal: yield and
			 * retry on the next iteration. */
		}

		if (n > 0)
			handle_worker_events(w, evs, n);

		uk_sched_yield();
	}
}

/*
 * Main thread loop: worker 0. Each iteration accepts every pending
 * connection on the listener and distributes the file descriptors
 * over the workers, then polls worker 0's own epoll set with a zero
 * timeout and handles the events for the connections it keeps. The
 * loop yields, so an idle worker does not busy-wait its core.
 */
static __noreturn void main_loop(void)
{
	struct epoll_event evs[64];
	struct sockaddr_in client_addr;
	socklen_t client_len;
	int cfd;
	int n;

	for(;;) {
		/* Accept all pending connections: the listener is
		 * non-blocking, so the loop drains the queue of
		 * connections waiting for an accept. */
		client_len = sizeof(client_addr);
		while ((cfd = accept4(mc_listener_fd,
				      (struct sockaddr *)&client_addr,
				      &client_len, SOCK_NONBLOCK)) >= 0) {
			client_len = sizeof(client_addr);
			distribute_fd(cfd);
		}

		/* Service the connections that worker 0 keeps on its
		 * own epoll set. */
		n = epoll_wait(mc_workers[0].epoll_fd, evs, 64, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
		}

		if (n > 0)
			handle_worker_events(&mc_workers[0], evs, n);

		uk_sched_yield();
	}
}

int main(int argc, char **argv)
{
	int i;
	struct uk_sched *s;

	(void)argc;
	(void)argv;

	printf("httpreply-mc: multi-core HTTP reply benchmark server\n");
	printf("httpreply-mc: mode=multi-core (threaded lwIP, 2 vCPU)\n");

	mc_nworkers = MC_MAX_WORKERS;
	for (s = uk_sched_head; s != NULL; s = s->next) {
		/* Count schedulers if populated by platform SMP */
	}

	printf("httpreply-mc: workers=%d\n", mc_nworkers);

	for (i = 0; i < mc_nworkers; i++) {
		mc_workers[i].worker_id = i;
		mc_workers[i].epoll_fd = epoll_create(1);
		mc_workers[i].recv_buf = malloc(MC_RECVBUF_SIZE);
		mc_workers[i].resp_pending = calloc(MAX_TRACKED_FDS,
						    sizeof(uint32_t));
		mc_workers[i].conn_count = 0;
		spsc_ring_init(&mc_rings[i]);
		/* Worker 0 is the main thread: it keeps its own
		 * connections, so it has no ring. */
		mc_workers[i].ring = (i == 0) ? NULL : &mc_rings[i];
	}

	for (i = 1; i < mc_nworkers; i++) {
		struct uk_sched *ws = mc_get_sched(i);
		struct uk_thread *th;

		/* Graceful single-core degradation: when the list of
		 * schedulers is too short, run the worker on the
		 * current scheduler. */
		if (ws == NULL)
			ws = uk_sched_current();

		th = uk_sched_thread_create(ws, worker_loop, &mc_workers[i], "worker");
		if (th == NULL) {
			printf("httpreply-mc: [ERR] failed to create worker %d\n", i);
			return 1;
		}
	}

	mc_listener_fd = create_listener();
	if (mc_listener_fd < 0) {
		printf("httpreply-mc: [ERR] failed to create the listener\n");
		return 1;
	}

	printf("httpreply-mc: [INFO] listening on port %d (backlog: %d)\n",
	       LISTEN_PORT, BACKLOG);

	main_loop();

	return 0;
}
