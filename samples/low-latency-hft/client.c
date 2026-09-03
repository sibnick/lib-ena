/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Low-Latency UDP Echo Benchmark Client (C lang)
 *
 * Transmits timestamped probe packets, waits for echoes, and computes
 * round-trip time (RTT) statistics with percentile latency distributions.
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
#include <time.h>
#include <math.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__linux__)
#include <sched.h>
#include <pthread.h>
#endif

#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_PORT 9000
#define DEFAULT_COUNT 10000
#define DEFAULT_PKT_SIZE 64
#define RECV_TIMEOUT_MS 200

struct __attribute__((packed)) probe_pkt {
	uint64_t seq;
	uint64_t send_ts_ns;
};

static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__linux__)
static int pin_to_core(int core_id)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif

static int compare_u64(const void *a, const void *b)
{
	uint64_t arg1 = *(const uint64_t *)a;
	uint64_t arg2 = *(const uint64_t *)b;
	if (arg1 < arg2) return -1;
	if (arg1 > arg2) return 1;
	return 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -s, --server <ip>        Server IP address (default: %s)\n", DEFAULT_SERVER_IP);
	printf("  -p, --port <port>        Server UDP port (default: %d)\n", DEFAULT_PORT);
	printf("  -n, --count <num>        Number of packets to probe (default: %d)\n", DEFAULT_COUNT);
	printf("  -l, --len <bytes>        Packet length in bytes (default: %d)\n", DEFAULT_PKT_SIZE);
	printf("  -r, --rate <pps>         Target send rate (pps, 0 = unthrottled)\n");
	printf("  -c, --core <id>          Pin client process to CPU core ID\n");
	printf("  -h, --help               Show this help message\n");
}

int main(int argc, char *argv[])
{
	const char *server_ip = DEFAULT_SERVER_IP;
	int port = DEFAULT_PORT;
	uint64_t count = DEFAULT_COUNT;
	size_t pkt_len = DEFAULT_PKT_SIZE;
	uint64_t rate_pps = 0;
	int pin_core = -1;

	static struct option long_options[] = {
		{"server", required_argument, 0, 's'},
		{"port",   required_argument, 0, 'p'},
		{"count",  required_argument, 0, 'n'},
		{"len",    required_argument, 0, 'l'},
		{"rate",   required_argument, 0, 'r'},
		{"core",   required_argument, 0, 'c'},
		{"help",   no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "s:p:n:l:r:c:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 's':
			server_ip = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			if (port <= 0 || port > 65535) {
				fprintf(stderr, "Error: Invalid port %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'n':
			count = (uint64_t)strtoull(optarg, NULL, 10);
			if (count == 0) {
				fprintf(stderr, "Error: Count must be > 0\n");
				return EXIT_FAILURE;
			}
			break;
		case 'l':
			pkt_len = (size_t)strtoul(optarg, NULL, 10);
			if (pkt_len < sizeof(struct probe_pkt)) {
				fprintf(stderr, "Error: Minimum packet size is %zu bytes\n",
					sizeof(struct probe_pkt));
				return EXIT_FAILURE;
			}
			break;
		case 'r':
			rate_pps = (uint64_t)strtoull(optarg, NULL, 10);
			break;
		case 'c':
			pin_core = atoi(optarg);
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

#if defined(__linux__)
	if (pin_core >= 0) {
		if (pin_to_core(pin_core) == 0) {
			printf("Pinned client process to CPU core %d\n", pin_core);
		} else {
			fprintf(stderr, "Warning: Failed to pin to CPU core %d\n", pin_core);
		}
	}
#endif

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	/* Socket timeout for receiving responses */
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = RECV_TIMEOUT_MS * 1000;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("setsockopt(SO_RCVTIMEO)");
	}

	int buf_size = 4 * 1024 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

	struct sockaddr_in dest_addr;
	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, server_ip, &dest_addr.sin_addr) <= 0) {
		fprintf(stderr, "Error: Invalid server address '%s'\n", server_ip);
		close(fd);
		return EXIT_FAILURE;
	}

	uint8_t *send_buf = malloc(pkt_len);
	uint8_t *recv_buf = malloc(pkt_len + 128);
	uint64_t *latencies = malloc(count * sizeof(uint64_t));

	if (!send_buf || !recv_buf || !latencies) {
		fprintf(stderr, "Error: Memory allocation failed\n");
		close(fd);
		return EXIT_FAILURE;
	}

	memset(send_buf, 0xAB, pkt_len);

	printf("========================================\n");
	printf(" Low-Latency UDP Echo Benchmark Client\n");
	printf(" Target: %s:%d\n", server_ip, port);
	printf(" Probes: %lu packets, Size: %zu bytes\n", (unsigned long)count, pkt_len);
	if (rate_pps > 0) {
		printf(" Target Rate: %lu pps\n", (unsigned long)rate_pps);
	} else {
		printf(" Target Rate: Unthrottled (RTT synchronous)\n");
	}
	printf("========================================\n\n");

	uint64_t interval_ns = rate_pps > 0 ? (1000000000ULL / rate_pps) : 0;
	uint64_t valid_echoes = 0;
	uint64_t dropped_packets = 0;
	uint64_t benchmark_start = get_time_ns();

	for (uint64_t seq = 0; seq < count; seq++) {
		struct probe_pkt *hdr = (struct probe_pkt *)send_buf;
		hdr->seq = seq;
		hdr->send_ts_ns = get_time_ns();

		ssize_t sent = sendto(fd, send_buf, pkt_len, 0,
				      (struct sockaddr *)&dest_addr, sizeof(dest_addr));
		if (sent != (ssize_t)pkt_len) {
			dropped_packets++;
			continue;
		}

		struct sockaddr_in reply_addr;
		socklen_t reply_len = sizeof(reply_addr);
		ssize_t recvd = recvfrom(fd, recv_buf, pkt_len + 128, 0,
					 (struct sockaddr *)&reply_addr, &reply_len);
		uint64_t now = get_time_ns();

		if (recvd > 0 && recvd >= (ssize_t)sizeof(struct probe_pkt)) {
			struct probe_pkt *reply_hdr = (struct probe_pkt *)recv_buf;
			if (reply_hdr->seq == seq) {
				uint64_t rtt = now - reply_hdr->send_ts_ns;
				latencies[valid_echoes++] = rtt;
			} else {
				/* Out-of-order or corrupt sequence */
				dropped_packets++;
			}
		} else {
			/* Timeout or drop */
			dropped_packets++;
		}

		if (interval_ns > 0) {
			uint64_t elapsed = get_time_ns() - hdr->send_ts_ns;
			if (elapsed < interval_ns) {
				struct timespec req;
				uint64_t rem = interval_ns - elapsed;
				req.tv_sec = (time_t)(rem / 1000000000ULL);
				req.tv_nsec = (long)(rem % 1000000000ULL);
				nanosleep(&req, NULL);
			}
		}
	}

	uint64_t benchmark_end = get_time_ns();
	double total_sec = (double)(benchmark_end - benchmark_start) / 1e9;

	close(fd);
	free(send_buf);
	free(recv_buf);

	if (valid_echoes == 0) {
		fprintf(stderr, "Failure: No valid echo responses received (%lu sent, %lu dropped).\n",
			(unsigned long)count, (unsigned long)dropped_packets);
		free(latencies);
		return EXIT_FAILURE;
	}

	qsort(latencies, valid_echoes, sizeof(uint64_t), compare_u64);

	uint64_t min_ns = latencies[0];
	uint64_t max_ns = latencies[valid_echoes - 1];
	uint64_t p50_ns = latencies[(size_t)(valid_echoes * 0.50)];
	uint64_t p90_ns = latencies[(size_t)(valid_echoes * 0.90)];
	uint64_t p99_ns = latencies[(size_t)(valid_echoes * 0.99)];
	uint64_t p999_ns = latencies[(size_t)(valid_echoes * 0.999)];

	uint64_t sum_ns = 0;
	for (uint64_t i = 0; i < valid_echoes; i++) {
		sum_ns += latencies[i];
	}
	double mean_ns = (double)sum_ns / (double)valid_echoes;

	double var_sum = 0.0;
	for (uint64_t i = 0; i < valid_echoes; i++) {
		double diff = (double)latencies[i] - mean_ns;
		var_sum += diff * diff;
	}
	double stddev_ns = sqrt(var_sum / (double)valid_echoes);

	double pps = (double)valid_echoes / total_sec;
	double loss_pct = (double)dropped_packets / (double)count * 100.0;

	printf("--- Benchmark Results ---\n");
	printf("Sent:               %lu packets\n", (unsigned long)count);
	printf("Received:           %lu packets (%.2f%% loss)\n",
	       (unsigned long)valid_echoes, loss_pct);
	printf("Duration:           %.3f seconds (%.0f pkts/sec)\n", total_sec, pps);
	printf("Round-Trip Latency (RTT):\n");
	printf("  Min:              %7.2f µs (%lu ns)\n", min_ns / 1000.0, (unsigned long)min_ns);
	printf("  p50 (Median):     %7.2f µs (%lu ns)\n", p50_ns / 1000.0, (unsigned long)p50_ns);
	printf("  p90:              %7.2f µs (%lu ns)\n", p90_ns / 1000.0, (unsigned long)p90_ns);
	printf("  p99:              %7.2f µs (%lu ns)\n", p99_ns / 1000.0, (unsigned long)p99_ns);
	printf("  p99.9:            %7.2f µs (%lu ns)\n", p999_ns / 1000.0, (unsigned long)p999_ns);
	printf("  Max:              %7.2f µs (%lu ns)\n", max_ns / 1000.0, (unsigned long)max_ns);
	printf("  Mean ± StdDev:    %7.2f ± %.2f µs\n", mean_ns / 1000.0, stddev_ns / 1000.0);
	printf("-------------------------\n");

	free(latencies);
	return (dropped_packets > 0 && valid_echoes < count * 0.99) ? EXIT_FAILURE : EXIT_SUCCESS;
}
