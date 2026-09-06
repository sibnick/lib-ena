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
 * Unit test for the lock-free SPSC ring buffer in
 * samples/httpreply-mc/spsc.h. Runs on the host with pthreads:
 * a real producer thread and a real consumer thread share one
 * ring, which exercises the acquire/release ordering end to end.
 */

#include "spsc.h"

#include <pthread.h>
#include <stdio.h>

static int g_failures;

#define SUBTEST(fn) do { \
	printf("[TEST] %s...\n", #fn); \
	if ((fn)() == 0) \
		printf("[PASS] %s\n", #fn); \
	else { \
		printf("[FAIL] %s\n", #fn); \
		g_failures++; \
	} \
} while (0)

#define TEST_ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "  assertion failed: %s\n", #cond); \
		return 1; \
	} \
} while (0)

/* 1. A fresh ring is empty and pop fails on it. */
static int test_empty_pop(void)
{
	struct spsc_ring ring;
	int fd;

	spsc_ring_init(&ring);

	TEST_ASSERT(spsc_ring_empty(&ring));
	TEST_ASSERT(spsc_ring_pop(&ring, &fd) == false);

	return 0;
}

/* 2. 1000 distinct fds come back in the exact order pushed. */
static int test_push_pop_fifo(void)
{
	struct spsc_ring ring;
	int fd;
	int i;

	spsc_ring_init(&ring);

	for (i = 0; i < 1000; i++)
		TEST_ASSERT(spsc_ring_push(&ring, 1000 + i));

	for (i = 0; i < 1000; i++) {
		TEST_ASSERT(spsc_ring_pop(&ring, &fd));
		TEST_ASSERT(fd == 1000 + i);
	}

	TEST_ASSERT(spsc_ring_empty(&ring));

	return 0;
}

/* 3. The ring holds SPSC_CAP items: the first SPSC_CAP pushes
 *    succeed, and the next push fails without any pops. */
static int test_full(void)
{
	struct spsc_ring ring;
	int i;

	spsc_ring_init(&ring);

	for (i = 0; i < SPSC_CAP + 1; i++) {
		if (!spsc_ring_push(&ring, i))
			break;
	}

	TEST_ASSERT(i == SPSC_CAP);
	TEST_ASSERT(spsc_ring_full(&ring));
	TEST_ASSERT(spsc_ring_push(&ring, SPSC_CAP + 1) == false);

	return 0;
}

/* 4. 10000 push/pop cycles wrap the 1024-slot buffer modulo. */
static int test_wraparound_single_thread(void)
{
	struct spsc_ring ring;
	int fd;
	int i;

	spsc_ring_init(&ring);

	for (i = 0; i < 10000; i++) {
		TEST_ASSERT(spsc_ring_push(&ring, i));
		TEST_ASSERT(spsc_ring_pop(&ring, &fd));
		TEST_ASSERT(fd == i);
	}

	return 0;
}

/* 5. Real cross-thread producer/consumer: a producer pthread
 *    pushes N fds while the main thread pops and verifies the
 *    exact sequence. */
#define CC_N 20000

static struct spsc_ring cc_ring;
static volatile int cc_done;

static void *cc_producer(void *arg)
{
	int i;

	(void)arg;

	for (i = 0; i < CC_N; i++) {
		while (!spsc_ring_push(&cc_ring, i))
			;
	}

	cc_done = 1;
	return NULL;
}

static int test_concurrent_producer_consumer(void)
{
	pthread_t prod;
	int expected;
	int fd;

	spsc_ring_init(&cc_ring);
	cc_done = 0;

	TEST_ASSERT(pthread_create(&prod, NULL, cc_producer, NULL) == 0);

	expected = 0;
	while (expected < CC_N) {
		if (spsc_ring_pop(&cc_ring, &fd)) {
			TEST_ASSERT(fd == expected);
			expected++;
		} else if (cc_done && spsc_ring_empty(&cc_ring)) {
			break;
		}
	}

	TEST_ASSERT(pthread_join(prod, NULL) == 0);
	TEST_ASSERT(expected == CC_N);

	return 0;
}

int main(void)
{
	printf("========================================\n");
	printf("Running SPSC Ring Test Suite\n");
	printf("========================================\n");

	SUBTEST(test_empty_pop);
	SUBTEST(test_push_pop_fifo);
	SUBTEST(test_full);
	SUBTEST(test_wraparound_single_thread);
	SUBTEST(test_concurrent_producer_consumer);

	printf("========================================\n");
	if (g_failures == 0) {
		printf("ALL SPSC TESTS PASSED (5/5)\n");
		printf("========================================\n");
		return 0;
	}

	printf("SPSC TESTS FAILED (%d of 5 failed)\n", g_failures);
	printf("========================================\n");
	return 1;
}
