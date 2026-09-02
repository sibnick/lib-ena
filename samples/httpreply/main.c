/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Benchmark Server (app-httpreply)
 *
 * Micro-benchmark HTTP echo server running on native AWS ENA driver
 * and lwIP TCP/IP stack in single-threaded (NO_SYS) mode.
 *
 * The lwIP stack runs without a dedicated stack thread. This one
 * thread does everything: it polls the network device
 * (uknetdev_poll_all()), drives the stack timers (sys_check_timeouts()),
 * and multiplexes sockets with level-triggered epoll. There are no
 * worker threads, no mailboxes, and no context switches between
 * device, stack, and application.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <lwip/timeouts.h>
#include "netif/uknetdev.h"

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define LISTEN_PORT 80
#define BACKLOG 512
#define MAX_EVENTS 256
#define RECV_BUF_SIZE 4096
#define SOCK_BUF_SIZE 32768

/*
 * When no socket is ready, pause the CPU for this long (nanoseconds)
 * before polling the device again. A network interrupt wakes the CPU
 * early, so the added latency is bounded by this value.
 */
#define IDLE_SLEEP_NS (100 * 1000)

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-ENA-Benchmark\r\n"
	"\r\n"
	"Hello, World!\n";

#include <sys/ioctl.h>

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
 * Drive the lwIP stack: receive pending packets from the network
 * device and process pending stack timers (retransmits, polls).
 */
static void drive_stack(void)
{
	uknetdev_poll_all();
	sys_check_timeouts();
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	struct sockaddr_in server_addr;
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	char buffer[RECV_BUF_SIZE];
	struct timespec idle_ts;
	int epfd, server_fd;
	int opt = 1;
	int n, i;
	size_t resp_len = sizeof(http_response) - 1;

	printf("\n========================================\n");
	printf(" Unikraft HTTP Benchmark Server (lib-ena)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
	printf(" Mode: Single-threaded (NO_SYS, epoll)\n");
	printf(" Driver: AWS ENA native netdev\n");
	printf(" Stack: lwIP (IPv4/TCP/Sockets)\n");
	printf("========================================\n\n");

	server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd < 0) {
		printf("[ERR] Failed to create socket: errno %d\n", errno);
		return 1;
	}

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		printf("[ERR] Failed to set SO_REUSEADDR: errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	configure_socket_options(server_fd);

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(LISTEN_PORT);

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		printf("[ERR] Failed to bind socket: errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	if (listen(server_fd, BACKLOG) < 0) {
		printf("[ERR] Failed to listen on socket: errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("[ERR] Failed to create epoll: errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("[ERR] Failed to register listener on epoll: errno %d\n", errno);
		close(server_fd);
		return 1;
	}

	idle_ts.tv_sec = 0;
	idle_ts.tv_nsec = IDLE_SLEEP_NS;

	printf("[INFO] HTTP server listening on port %d (backlog: %d)...\n",
	       LISTEN_PORT, BACKLOG);

	for (;;) {
		/* Receive packets and run stack timers first, so that
		 * events raised by the stack are already visible to
		 * epoll before we query it. */
		drive_stack();

		/* Non-blocking: timeout 0. */
		n = epoll_wait(epfd, events, MAX_EVENTS, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("[ERR] epoll_wait error: errno %d\n", errno);
			break;
		}

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == server_fd) {
				/* Accept all pending connections */
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
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd,
						      &ev) < 0) {
						close(cfd);
						break;
					}
				}
				continue;
			}

			if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				/* Peer closed the connection or an error
				 * happened: drop the connection. */
				epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
				continue;
			}

			if (events[i].events & (EPOLLIN | EPOLLRDNORM)) {
				ssize_t bytes_read = recv(fd, buffer, sizeof(buffer) - 1, 0);
				if (bytes_read > 0) {
					buffer[bytes_read] = '\0';
					send(fd, http_response, resp_len, 0);
					if (strstr(buffer, "Connection: close") != NULL ||
					    strstr(buffer, "connection: close") != NULL) {
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
					}
				} else if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					close(fd);
				}
			}
		}

		if (n == 0) {
#if defined(__x86_64__)
			__asm__ __volatile__("pause");
#endif
		}
	}

	close(server_fd);
	close(epfd);

	return 0;
}
