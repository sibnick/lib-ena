/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Low-Latency UDP Echo Server (C lang)
 *
 * Designed for low-latency packet processing and round-trip time (RTT)
 * measurement. Supports both standard Linux and Unikraft unikernels.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__linux__) && !defined(__Unikraft__)
#include <sched.h>
#include <pthread.h>
#endif

#if defined(__Unikraft__)
#include <lwip/timeouts.h>
#include "netif/uknetdev.h"
#endif

#define DEFAULT_PORT 9000
#define DEFAULT_BIND_IP "0.0.0.0"
#define BUFFER_SIZE 65536
#define SOCKET_BUFFER_SIZE (4 * 1024 * 1024)

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

#if defined(__linux__) && !defined(__Unikraft__)
static int pin_to_core(int core_id)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCKET_BUFFER_SIZE;

	/* Allow quick address and port reuse */
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		perror("setsockopt(SO_REUSEADDR)");
	}

#ifdef SO_REUSEPORT
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		perror("setsockopt(SO_REUSEPORT)");
	}
#endif

	/* Increase OS receive and send buffers to avoid drops */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) < 0) {
		perror("setsockopt(SO_RCVBUF)");
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) < 0) {
		perror("setsockopt(SO_SNDBUF)");
	}

	/* Set low-delay IP TOS (DSCP CS4 / low delay) */
#ifdef IP_TOS
	int tos = 0x10; /* IPTOS_LOWDELAY */
	if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
		/* Not fatal */
	}
#endif

	/* Enable busy polling if supported by Linux kernel */
#ifdef SO_BUSY_POLL
	int busy_poll_us = 50;
	if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us)) < 0) {
		/* Not fatal: ignore if unsupported */
	}
#endif
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -p, --port <port>        UDP port to bind (default: %d)\n", DEFAULT_PORT);
	printf("  -b, --bind <ip>          IP address to bind (default: %s)\n", DEFAULT_BIND_IP);
	printf("  -c, --core <id>          Pin server process to CPU core ID\n");
	printf("  -v, --verbose            Print packet activity\n");
	printf("  -h, --help               Show this help message\n");
}

int main(int argc, char *argv[])
{
	int port = DEFAULT_PORT;
	const char *bind_ip = DEFAULT_BIND_IP;
	int pin_core = -1;
	bool verbose = false;

	static struct option long_options[] = {
		{"port",    required_argument, 0, 'p'},
		{"bind",    required_argument, 0, 'b'},
		{"core",    required_argument, 0, 'c'},
		{"verbose", no_argument,       0, 'v'},
		{"help",    no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "p:b:c:vh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			if (port <= 0 || port > 65535) {
				fprintf(stderr, "Error: Invalid port %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'b':
			bind_ip = optarg;
			break;
		case 'c':
			pin_core = atoi(optarg);
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

#if defined(__linux__) && !defined(__Unikraft__)
	if (pin_core >= 0) {
		if (pin_to_core(pin_core) == 0) {
			printf("Pinned server process to CPU core %d\n", pin_core);
		} else {
			fprintf(stderr, "Warning: Failed to pin to CPU core %d\n", pin_core);
		}
	}
#endif

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sa.sa_flags = 0; /* Do NOT set SA_RESTART so blocking recvfrom is interrupted */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	/* Periodic receive timeout to ensure loop checks g_running */
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 200000; /* 200ms */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	configure_socket_options(fd);

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons((uint16_t)port);

	if (inet_pton(AF_INET, bind_ip, &server_addr.sin_addr) <= 0) {
		fprintf(stderr, "Error: Invalid bind address '%s'\n", bind_ip);
		close(fd);
		return EXIT_FAILURE;
	}

	if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind");
		close(fd);
		return EXIT_FAILURE;
	}

	printf("========================================\n");
	printf(" Low-Latency UDP Echo Server\n");
	printf(" Listening on: %s:%d\n", bind_ip, port);
	printf(" Mode: Zero-copy direct echo\n");
	printf("========================================\n");
	fflush(stdout);

	uint8_t buffer[BUFFER_SIZE];
	struct sockaddr_in client_addr;
	socklen_t client_len;
	uint64_t rx_packets = 0;
	uint64_t tx_packets = 0;
	uint64_t rx_bytes = 0;

	while (g_running) {
#if defined(__Unikraft__)
		uknetdev_poll_all();
		sys_check_timeouts();
#endif
		client_len = sizeof(client_addr);
		ssize_t n = recvfrom(fd, buffer, sizeof(buffer), 0,
				     (struct sockaddr *)&client_addr, &client_len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			perror("recvfrom");
			break;
		}

		rx_packets++;
		rx_bytes += (uint64_t)n;

		/* In-place echo back to sender */
		ssize_t sent = sendto(fd, buffer, (size_t)n, 0,
				      (struct sockaddr *)&client_addr, client_len);
		if (sent > 0) {
			tx_packets++;
		} else if (sent < 0 && errno != EINTR) {
			if (verbose) {
				perror("sendto");
			}
		}

		if (verbose && (rx_packets % 10000 == 0 || rx_packets <= 10)) {
			char client_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &client_addr.sin_addr, client_str, sizeof(client_str));
			printf("Echoed %zd bytes to %s:%d (total rx: %lu)\n",
			       n, client_str, ntohs(client_addr.sin_port), (unsigned long)rx_packets);
			fflush(stdout);
		}
	}

	printf("\nServer shutdown complete.\n");
	printf("Total packets received: %lu\n", (unsigned long)rx_packets);
	printf("Total packets echoed:   %lu\n", (unsigned long)tx_packets);
	printf("Total bytes processed:  %lu\n", (unsigned long)rx_bytes);

	close(fd);
	return EXIT_SUCCESS;
}
