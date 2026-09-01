/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Benchmark Server (app-httpreply)
 *
 * Micro-benchmark HTTP echo server running on native AWS ENA driver
 * and lwIP TCP/IP stack with asynchronous non-blocking I/O multiplexing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define LISTEN_PORT 80
#define BACKLOG 512
#define MAX_CLIENTS 1024
#define RECV_BUF_SIZE 4096
#define SOCK_BUF_SIZE 32768

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-ENA-Benchmark\r\n"
	"\r\n"
	"Hello, World!\n";

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCK_BUF_SIZE;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	int server_fd;
	struct sockaddr_in server_addr;
	int opt = 1;
	struct pollfd fds[MAX_CLIENTS];
	nfds_t nfds = 1;
	char buffer[RECV_BUF_SIZE];
	size_t resp_len = sizeof(http_response) - 1;

	printf("\n========================================\n");
	printf(" Unikraft HTTP Benchmark Server (lib-ena)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
	printf(" Mode: Asynchronous Non-blocking (poll)\n");
	printf(" Driver: AWS ENA native netdev\n");
	printf(" Stack: lwIP (IPv4/TCP/Sockets)\n");
	printf("========================================\n\n");

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
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
	if (set_nonblocking(server_fd) < 0) {
		printf("[ERR] Failed to set non-blocking on server socket\n");
		close(server_fd);
		return 1;
	}

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

	memset(fds, 0, sizeof(fds));
	fds[0].fd = server_fd;
	fds[0].events = POLLIN;

	printf("[INFO] HTTP server listening on port %d (max clients: %d)...\n", LISTEN_PORT, MAX_CLIENTS);

	while (1) {
		int nready = poll(fds, nfds, 100);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			printf("[ERR] poll error: errno %d\n", errno);
			break;
		}

		if (nready == 0)
			continue;

		/* Check for new incoming connections */
		if (fds[0].revents & POLLIN) {
			while (1) {
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

				if (client_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
						break;
					break;
				}

				if (nfds >= MAX_CLIENTS) {
					close(client_fd);
					break;
				}

				set_nonblocking(client_fd);
				configure_socket_options(client_fd);

				fds[nfds].fd = client_fd;
				fds[nfds].events = POLLIN;
				fds[nfds].revents = 0;
				nfds++;
			}
		}

		/* Process active client connections */
		for (nfds_t i = 1; i < nfds; i++) {
			int cfd = fds[i].fd;

			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				close(cfd);
				fds[i] = fds[nfds - 1];
				nfds--;
				i--;
				continue;
			}

			if (fds[i].revents & POLLIN) {
				ssize_t bytes_read;
				int should_close = 0;

				while ((bytes_read = recv(cfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
					buffer[bytes_read] = '\0';

					ssize_t sent = send(cfd, http_response, resp_len, 0);
					if (sent < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						should_close = 1;
						break;
					}

					if (strstr(buffer, "Connection: close") != NULL ||
					    strstr(buffer, "connection: close") != NULL) {
						should_close = 1;
						break;
					}
				}

				if (bytes_read == 0 || should_close ||
				    (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
					close(cfd);
					fds[i] = fds[nfds - 1];
					nfds--;
					i--;
				}
			}
		}
	}

	for (nfds_t i = 0; i < nfds; i++)
		close(fds[i].fd);

	return 0;
}
