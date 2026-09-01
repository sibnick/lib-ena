/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Simple "Hello World" HTTP Web Server for Unikraft Unikernel with AWS ENA.
 *
 * Threading model: a single thread.  The main thread runs an epoll event
 * loop over non-blocking sockets: the listening fd signals EPOLLIN while
 * connections are pending in the accept queue, a connection fd signals
 * EPOLLIN when request bytes are available and EPOLLOUT when the TCP send
 * buffer has room again.  All poll-queue levels are maintained by the lwIP
 * "tcpip" thread through the UK file layer (managed ukfile poll queues),
 * so the reactor parks in epoll_wait() and only runs when there is work.
 *
 * Connections live in a fixed static table of 8 KiB request buffers; no
 * per-connection heap allocation is done (the unikernel allocator is not
 * locked and only the reactor thread would use it here anyway).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <lwip/ip_addr.h>

#define SERVER_PORT 80

/* Per-connection request buffer size (fits typical request headers) */
#define BUF_SIZE 8192

/* Fixed table of simultaneous connections (~1 MiB of BSS) */
#define MAX_CONNS 128

/* Max events served per epoll_wait() wakeup */
#define MAX_EVENTS 32

enum { CONN_READING, CONN_WRITING };

static const char HTTP_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 13\r\n"
    "Connection: close\r\n"
    "Server: Unikraft-ENA/1.0\r\n"
    "\r\n"
    "Hello, World!";

#define RESP_LEN (sizeof(HTTP_RESPONSE) - 1)

struct conn {
	int fd;
	int state;
	size_t rlen;
	size_t woff;
	char rbuf[BUF_SIZE];
};

static struct conn conns[MAX_CONNS];
static int nconns;
static int epfd;
static int listener_fd;
static bool listener_armed;

static void netif_status_cb(struct netif *nf)
{
	printf("[INFO] Network Interface %c%c%u updated: IP=%s, link=%d, up=%d\n",
	       nf->name[0], nf->name[1], nf->num,
	       ip4addr_ntoa(netif_ip4_addr(nf)),
	       netif_is_link_up(nf), netif_is_up(nf));
}

static void arm_listener(void)
{
	struct epoll_event ev = { .events = EPOLLIN,
				  .data.fd = listener_fd };

	if (listener_fd >= 0 && !listener_armed) {
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener_fd, &ev) == 0)
			listener_armed = true;
	}
}

/* Stop reporting the listener until arm_listener() re-adds it (prevents a
 * busy loop while the connection table is full or accept keeps failing) */
static void disarm_listener(void)
{
	if (listener_fd >= 0 && listener_armed) {
		(void)epoll_ctl(epfd, EPOLL_CTL_DEL, listener_fd, NULL);
		listener_armed = false;
	}
}

static struct conn *conn_alloc(int fd)
{
	int i;

	for (i = 0; i < MAX_CONNS; i++) {
		if (conns[i].fd < 0) {
			conns[i].fd = fd;
			conns[i].state = CONN_READING;
			conns[i].rlen = 0;
			conns[i].woff = 0;
			nconns++;
			return &conns[i];
		}
	}
	return NULL;
}

static struct conn *conn_lookup(int fd)
{
	int i;

	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i].fd == fd)
			return &conns[i];
	return NULL;
}

static void conn_teardown(struct conn *c)
{
	if (c->fd < 0)
		return;
	(void)epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
	nconns--;
	/* Re-arm the listener if we stopped accepting due to a full table */
	arm_listener();
}

/* Write the remainder of the response; on a short write re-register the
 * fd with EPOLLOUT (woken when lwIP's send buffer drains).  Tears the
 * connection down when the response is complete or on error.
 */
static void conn_send(struct conn *c)
{
	ssize_t n;

	n = write(c->fd, HTTP_RESPONSE + c->woff, RESP_LEN - c->woff);
	if (n > 0) {
		c->woff += (size_t)n;
		if (c->woff >= RESP_LEN) {
			printf("[  OK] conn fd=%d: response sent\n", c->fd);
			conn_teardown(c);
			return;
		}
	} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		struct epoll_event ev = { .events = EPOLLOUT,
					  .data.fd = c->fd };

		if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0)
			conn_teardown(c);
		return;
	}

	if (n == 0)
		printf("[INFO] conn fd=%d: client closed during response\n",
		       c->fd);
	else
		printf("[ERR] conn fd=%d: write failed (%d)\n", c->fd, errno);
	conn_teardown(c);
}

/* Accumulate request bytes until the header terminator arrives; then
 * switch to CONN_WRITING and start sending the response.  Safe to call
 * when no data is pending (spurious/stale level): the read returns
 * -EAGAIN and we simply keep the EPOLLIN registration.
 */
static void conn_on_readable(struct conn *c)
{
	for (;;) {
		ssize_t n;

		if (c->rlen >= BUF_SIZE - 1) {
			c->state = CONN_WRITING;
			conn_send(c);
			return;
		}

		n = read(c->fd, c->rbuf + c->rlen, BUF_SIZE - 1 - c->rlen);
		if (n > 0) {
			c->rlen += (size_t)n;
			c->rbuf[c->rlen] = '\0';
			if (strstr(c->rbuf, "\r\n\r\n")) {
				c->state = CONN_WRITING;
				conn_send(c);
			}
			continue;
		}

		if (n == 0) {
			printf("[INFO] conn fd=%d: client closed before full request\n",
			       c->fd);
			conn_teardown(c);
			return;
		}

		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		printf("[ERR] conn fd=%d: read failed (%d)\n", c->fd, errno);
		conn_teardown(c);
		return;
	}
}

/* Drain pending connections from the listening socket.  On -EAGAIN the
 * accept queue is empty; on a hard error (or a full connection table) the
 * listener is unregistered to avoid a busy loop until a slot frees up.
 */
static void accept_pending(void)
{
	for (;;) {
		struct sockaddr_in client_addr;
		struct epoll_event ev;
		struct conn *c;
		socklen_t client_len = sizeof(client_addr);
		uint32_t ip;
		int cfd;

		cfd = accept(listener_fd, (struct sockaddr *)&client_addr,
			     &client_len);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				printf("[ERR] accept failed (%d), "
				       "stopping new connections\n", errno);
				disarm_listener();
			}
			return;
		}

		c = conn_alloc(cfd);
		if (!c) {
			printf("[WARN] connection limit reached, dropping connection\n");
			disarm_listener();
			close(cfd);
			return;
		}

		(void)fcntl(cfd, F_SETFL, O_NONBLOCK);
		ev.events = EPOLLIN;
		ev.data.fd = cfd;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
			printf("[ERR] epoll_ctl(ADD) failed for fd=%d, dropping\n",
			       cfd);
			disarm_listener();
			close(cfd);
			c->fd = -1;
			nconns--;
			return;
		}

		ip = ntohl(client_addr.sin_addr.s_addr);
		printf("[INFO] conn %d: accepted fd=%d from %u.%u.%u.%u:%d\n",
		       nconns, cfd,
		       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
		       ip & 0xFF, ntohs(client_addr.sin_port));
	}
}

int main(int argc __attribute__((unused)),
	 char *argv[] __attribute__((unused)))
{
	struct sockaddr_in server_addr;
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	struct netif *nf;
	int optval = 1;
	int i;

	printf("\n=====================================================\n");
	printf(" Unikraft Unikernel Web Server (AWS ENA Native Driver)\n");
	printf(" Target Platform: AWS EC2 (t3.nano)\n");
	printf("=====================================================\n\n");

	printf("[INFO] Inspecting network interfaces...\n");
	for (nf = netif_list; nf != NULL; nf = nf->next) {
		printf("[INFO] Interface %c%c%u: flags=0x%x, up=%d, ip=%s\n",
		       nf->name[0], nf->name[1], nf->num, nf->flags,
		       netif_is_up(nf),
		       ip4addr_ntoa(netif_ip4_addr(nf)));
		netif_set_status_callback(nf, netif_status_cb);
	}

	listener_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listener_fd < 0) {
		printf("[ERR] Failed to create socket\n");
		return 1;
	}

	if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
		       &optval, sizeof(optval)) < 0)
		printf("[WARN] Failed to set SO_REUSEADDR\n");

	if (fcntl(listener_fd, F_SETFL, O_NONBLOCK) < 0)
		printf("[WARN] Failed to set O_NONBLOCK on listener\n");

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(SERVER_PORT);

	if (bind(listener_fd, (struct sockaddr *)&server_addr,
		sizeof(server_addr)) < 0) {
		printf("[ERR] Failed to bind socket to port 80\n");
		close(listener_fd);
		return 1;
	}

	if (listen(listener_fd, 128) < 0) {
		printf("[ERR] Failed to listen on socket\n");
		close(listener_fd);
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("[ERR] Failed to create epoll fd\n");
		close(listener_fd);
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = listener_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener_fd, &ev) < 0) {
		printf("[ERR] Failed to register listener with epoll\n");
		close(epfd);
		close(listener_fd);
		return 1;
	}
	listener_armed = true;

	printf("[INFO] HTTP server listening on 0.0.0.0:%d\n", SERVER_PORT);
	printf("[INFO] Async: single-threaded epoll reactor, %d max conns\n",
	       MAX_CONNS);
	printf("[INFO] Ready for incoming HTTP requests!\n");

	for (;;) {
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		int i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("[ERR] epoll_wait failed (%d), exiting\n",
			       errno);
			break;
		}

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;
			unsigned int eflags = events[i].events;
			struct conn *c;

			if (fd == listener_fd) {
				if (eflags & (EPOLLERR | EPOLLHUP)) {
					printf("[ERR] listener error/hup (%#x), "
					       "stopping new connections\n",
					       eflags);
					disarm_listener();
					close(listener_fd);
					listener_fd = -1;
				} else if (eflags & EPOLLIN) {
					accept_pending();
				}
				continue;
			}

			c = conn_lookup(fd);
			if (!c)
				continue;

			if (eflags & (EPOLLERR | EPOLLHUP | EPOLLNVAL)) {
				printf("[INFO] conn fd=%d: %s\n", fd,
				       (eflags & EPOLLNVAL) ? "invalid fd" :
				       (eflags & EPOLLERR) ? "socket error" :
				       "peer closed");
				conn_teardown(c);
				continue;
			}

			if (eflags & EPOLLIN && c->state == CONN_READING) {
				conn_on_readable(c);
				if (c->fd < 0)
					continue;
			}

			if (c->state == CONN_WRITING && (eflags & EPOLLOUT))
				conn_send(c);
		}
	}

	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i].fd >= 0)
			conn_teardown(&conns[i]);
	if (listener_fd >= 0)
		close(listener_fd);
	close(epfd);
	return 0;
}
