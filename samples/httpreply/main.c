/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Benchmark Server (app-httpreply)
 *
 * Micro-benchmark HTTP echo server running on native AWS ENA driver
 * and lwIP TCP/IP stack.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTEN_PORT 80
#define BACKLOG 128
#define RECV_BUF_SIZE 4096

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-ENA-Benchmark\r\n"
	"\r\n"
	"Hello, World!\n";

static void handle_client(int client_fd)
{
	char buffer[RECV_BUF_SIZE];
	ssize_t bytes_read;
	size_t resp_len = sizeof(http_response) - 1;

	/* Keep-alive request loop */
	while ((bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
		buffer[bytes_read] = '\0';

		ssize_t sent = send(client_fd, http_response, resp_len, 0);
		if (sent < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			break;
		}

		/* If client requested Connection: close, exit loop */
		if (strstr(buffer, "Connection: close") != NULL ||
		    strstr(buffer, "connection: close") != NULL) {
			break;
		}
	}

	close(client_fd);
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	int server_fd;
	struct sockaddr_in server_addr;
	int opt = 1;

	printf("\n========================================\n");
	printf(" Unikraft HTTP Benchmark Server (lib-ena)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
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

	printf("[INFO] HTTP server listening on port %d...\n", LISTEN_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

		if (client_fd < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			printf("[ERR] Accept error: errno %d\n", errno);
			break;
		}

		handle_client(client_fd);
	}

	close(server_fd);
	return 0;
}
