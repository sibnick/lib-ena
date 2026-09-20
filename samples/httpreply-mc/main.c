/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Multi-Core Benchmark Server (app-httpreply-mc).
 *
 * Micro-benchmark HTTP echo server running on native AWS ENA driver
 * and lwIP TCP/IP stack in multi-core run-to-completion mode.
 *
 * Each core executes an isolated run-to-completion loop. Each core
 * polls its assigned hardware queue, handles stack timers, accepts
 * incoming connections via SO_REUSEPORT, and transmits responses
 * on its dedicated TX queue. There are no locks, no inter-core queues,
 * and no shared state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread.h>
#include <lwip/timeouts.h>
#include "netif/uknetdev.h"

#define MC_MAX_WORKERS 2
#define MC_RECVBUF_SIZE 4096

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#define LISTEN_PORT 80
#define BACKLOG 512
#define MAX_EVENTS 256
#define SOCK_BUF_SIZE 32768
#define MAX_TRACKED_FDS 2048

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
	int listener_fd;
	int conn_count;
	char *recv_buf;
	uint32_t *resp_pending;
	unsigned long req_count;
	unsigned long byte_count;
};

static struct worker_ctx mc_workers[MC_MAX_WORKERS];
static int mc_nworkers;

/*
 * Get the scheduler that owns vCPU idx by walking uk_sched_head,
 * the linked list of all per-LCPU schedulers. Index 0 is the BSP
 * (vCPU 0) and index 1 is vCPU 1.
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
 * Update the interest set of an fd registered with worker epoll instance.
 */
static int set_epoll_events(int epfd, int fd, uint32_t events)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.fd = fd;

	return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Drive the local core network stack: receive packets from dedicated
 * RX queue and process local stack timers.
 */
static void drive_core_stack(int core_id)
{
	uknetdev_poll_rxqueue((uint16_t)core_id);
	sys_check_timeouts();
}

/*
 * Send as many bytes of pending response as the socket accepts.
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
			      errno == ENOBUFS || errno == EBUSY)) {
			drive_core_stack(w->worker_id);
			n = send(fd, http_response + (http_resp_len - pending), pending, 0);
			if (n > 0) {
				pending -= (uint32_t)n;
				continue;
			}
			break;
		}

		return -1;
	}

	w->resp_pending[fd] = pending;

	if (pending > 0) {
		if (set_epoll_events(w->epoll_fd, fd, base_events | EPOLLOUT) < 0)
			return -1;
		return 1;
	}

	if (set_epoll_events(w->epoll_fd, fd, base_events) < 0)
		return -1;

	return 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from epoll,
 * and close the socket descriptor.
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
 * Create an independent listener socket on LISTEN_PORT with SO_REUSEPORT.
 * Each core creates its own listener inside its local lwIP state.
 */
static int create_core_listener(void)
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
		printf("httpreply-mc: [ERR] SO_REUSEADDR failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEPORT failed: errno %d\n", errno);
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
 * Run-to-completion worker engine. Bound to a single dedicated core
 * and its assigned hardware queue pair.
 */
static __noreturn void run_to_completion_worker(int core_id)
{
	struct worker_ctx *w = &mc_workers[core_id];
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	int server_fd;
	int n, i;

	server_fd = create_core_listener();
	if (server_fd < 0) {
		printf("httpreply-mc: [ERR] core %d failed to create listener\n",
		       core_id);
		for (;;) {
			uk_sched_yield();
		}
	}
	w->listener_fd = server_fd;

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("httpreply-mc: [ERR] core %d failed to add listener to epoll: errno %d\n",
		       core_id, errno);
		close(server_fd);
		for (;;) {
			uk_sched_yield();
		}
	}

	printf("httpreply-mc: [INFO] core %d listening on port %d (queue pair %d)\n",
	       core_id, LISTEN_PORT, core_id);

	for (;;) {
		drive_core_stack(core_id);

		n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("httpreply-mc: [ERR] core %d epoll_wait errno %d\n",
			       core_id, errno);
			continue;
		}

		for (i = 0; i < n; i++) {
			uint32_t base_events = EPOLLIN | EPOLLRDHUP;
			int fd = events[i].data.fd;

			if (fd == server_fd) {
				for (;;) {
					struct sockaddr_in client_addr;
					socklen_t client_len = sizeof(client_addr);
					int cfd;

					cfd = accept4(server_fd,
						      (struct sockaddr *)&client_addr,
						      &client_len, SOCK_NONBLOCK);
					if (cfd < 0)
						break;

					configure_socket_options(cfd);

					ev.events = EPOLLIN | EPOLLRDHUP;
					ev.data.fd = cfd;
					if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD,
						      cfd, &ev) < 0) {
						close(cfd);
						break;
					}
					w->conn_count++;
				}
				continue;
			}

			if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				drop_connection(w, fd);
				continue;
			}

			if (fd < MAX_TRACKED_FDS && w->resp_pending[fd] > 0) {
				if (events[i].events & EPOLLOUT) {
					if (send_pending_response(w, fd, base_events) < 0)
						drop_connection(w, fd);
				}
				continue;
			}

			if (events[i].events & (EPOLLIN | EPOLLRDNORM)) {
				ssize_t r = recv(fd, w->recv_buf, MC_RECVBUF_SIZE - 1, 0);

				if (r > 0) {
					w->recv_buf[r] = '\0';
					w->req_count++;
					w->byte_count += http_resp_len;
					if ((w->req_count % MC_STATS_INTERVAL) == 0)
						printf("httpreply-mc: [stats] core %d: req=%lu bytes=%lu\n",
						       core_id,
						       (unsigned long)w->req_count,
						       (unsigned long)w->byte_count);

					if (fd < MAX_TRACKED_FDS) {
						w->resp_pending[fd] = (uint32_t)http_resp_len;
						if (send_pending_response(w, fd, base_events) < 0)
							drop_connection(w, fd);
					} else {
						send(fd, http_response, http_resp_len, 0);
					}

					if (strstr(w->recv_buf, "Connection: close") != NULL ||
					    strstr(w->recv_buf, "connection: close") != NULL)
						drop_connection(w, fd);
				} else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
					drop_connection(w, fd);
				}
			}
		}

		if (n == 0) {
#if defined(__x86_64__)
			__asm__ __volatile__("pause");
#elif defined(__aarch64__)
			__asm__ __volatile__("yield");
#endif
		}
	}
}

static __noreturn void worker_thread(void *arg)
{
	struct worker_ctx *w = (struct worker_ctx *)arg;
	run_to_completion_worker(w->worker_id);
}

int main(int argc, char **argv)
{
	int i;
	struct uk_sched *s;

	(void)argc;
	(void)argv;

	printf("\n=============================================\n");
	printf(" Unikraft HTTP Benchmark Server (lib-ena-mc)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
	printf(" Mode: Shared-nothing run-to-completion (NO_SYS)\n");
	printf(" Driver: AWS ENA multi-queue\n");
	printf(" Stack: lwIP per-core state (SO_REUSEPORT)\n");
	printf("=============================================\n\n");

	mc_nworkers = 0;
	for (s = uk_sched_head; s != NULL && mc_nworkers < MC_MAX_WORKERS; s = s->next)
		mc_nworkers++;

	if (mc_nworkers < 1)
		mc_nworkers = 1;

	printf("httpreply-mc: detected %d worker cores\n", mc_nworkers);

	for (i = 0; i < mc_nworkers; i++) {
		mc_workers[i].worker_id = i;
		mc_workers[i].epoll_fd = epoll_create1(0);
		if (mc_workers[i].epoll_fd < 0) {
			printf("httpreply-mc: [ERR] epoll_create1 failed for worker %d\n", i);
			return 1;
		}
		mc_workers[i].recv_buf = malloc(MC_RECVBUF_SIZE);
		mc_workers[i].resp_pending = calloc(MAX_TRACKED_FDS, sizeof(uint32_t));
		mc_workers[i].conn_count = 0;
		mc_workers[i].req_count = 0;
		mc_workers[i].byte_count = 0;
	}

	for (i = 1; i < mc_nworkers; i++) {
		struct uk_sched *ws = mc_get_sched(i);
		struct uk_thread *th;

		if (ws == NULL)
			ws = uk_sched_current();

		th = uk_sched_thread_create(ws, worker_thread, &mc_workers[i], "worker");
		if (th == NULL) {
			printf("httpreply-mc: [ERR] failed to start worker %d\n", i);
			return 1;
		}
	}

	run_to_completion_worker(0);

	return 0;
}
