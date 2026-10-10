/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 *
 * Unit test for the idle backoff policy in
 * samples/httpreply-mc/idlebackoff.h. Runs on the host.
 *
 * The policy decides, per worker-loop iteration, whether a run-to-
 * completion core may sleep and for how long. The tests cover the
 * no-sleep window, the active regime, the dormant regime, the
 * exponential growth, the cap, and the reset on work.
 *
 * [Ticket 597c1b9731]
 */

#include "idlebackoff.h"

#include <stdio.h>
#include <stdint.h>

static int g_failures;

#define SUBTEST(fn)                                                            \
	do {                                                                   \
		printf("[TEST] %s...\n", #fn);                                 \
		if ((fn)() == 0)                                               \
			printf("[PASS] %s\n", #fn);                            \
		else {                                                         \
			printf("[FAIL] %s\n", #fn);                            \
			g_failures++;                                          \
		}                                                              \
	} while (0)

#define TEST_ASSERT(cond)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  assertion failed: %s (line %d)\n",  \
				#cond, __LINE__);                              \
			return 1;                                              \
		}                                                              \
	} while (0)

#define T0 1000000ULL /* 1 ms: time at which the core starts */

/* A fresh state takes no sleep. */
static int test_init_no_sleep(void)
{
	struct ib_state s;

	ib_init(&s, T0);

	TEST_ASSERT(s.streak == 0);
	TEST_ASSERT(s.shift == 0);
	TEST_ASSERT(s.last_work == T0);
	TEST_ASSERT(ib_tick(&s, T0 + 1000, 0) == 0);

	return 0;
}

/*
 * No sleep for the first IB_IDLE_STREAK_LIMIT no-work iterations,
 * then the fixed active sleep.
 */
static int test_active_streak_window(void)
{
	struct ib_state s;
	int i;

	ib_init(&s, T0);

	for (i = 0; i < (int)IB_IDLE_STREAK_LIMIT; i++)
		TEST_ASSERT(ib_tick(&s, T0 + (uint64_t)(i + 1) * 1000, 0) == 0);

	/* The 9th no-work iteration crosses the limit. */
	TEST_ASSERT(ib_tick(&s,
			    T0 + (uint64_t)(IB_IDLE_STREAK_LIMIT + 1) * 1000,
			    0) == IB_ACTIVE_SLEEP_NS);

	return 0;
}

/*
 * While work stays recent (within IB_ACTIVE_WINDOW_NS), the sleep
 * stays the short active sleep no matter how the streak grows.
 */
static int test_active_regime_stays_short(void)
{
	struct ib_state s;
	int i;

	ib_init(&s, T0);

	/*
	 * Tick 1 us apart for 1.5 ms: 1500 iterations, all no work.
	 * The gap to the last work reaches 1.5 ms, still below the
	 * 2 ms active window, so every sleep is the active one.
	 */
	for (i = 0; i < 1500; i++) {
		uint64_t ns = ib_tick(&s, T0 + (uint64_t)(i + 1) * 1000, 0);

		if ((uint32_t)(i + 1) <= IB_IDLE_STREAK_LIMIT)
			TEST_ASSERT(ns == 0);
		else
			TEST_ASSERT(ns == IB_ACTIVE_SLEEP_NS);
	}

	return 0;
}

/*
 * After the active window passes with no work, the sleep budget
 * grows exponentially and stops at the cap.
 */
static int test_dormant_growth_and_cap(void)
{
	struct ib_state s;
	int i;
	uint64_t expect[] = {
	    IB_LONG_BASE_NS, IB_LONG_BASE_NS << 1, IB_LONG_BASE_NS << 2,
	    IB_LONG_MAX_NS,  IB_LONG_MAX_NS,
	};

	/* Start ticking at 3 ms: already beyond the 2 ms window. */
	ib_init(&s, T0);

	/*
	 * Twelve ticks 1 us apart. Ticks 1 to 8 build the streak (no
	 * sleep). Ticks 9 to 12 enter the dormant regime and grow the
	 * budget 1, 2, 4, 8 ms.
	 */
	for (i = 0; i < 12; i++) {
		uint64_t now = T0 + 3000000ULL + (uint64_t)(i + 1) * 1000;
		uint64_t ns = ib_tick(&s, now, 0);

		if ((uint32_t)(i + 1) <= IB_IDLE_STREAK_LIMIT)
			TEST_ASSERT(ns == 0);
		else
			TEST_ASSERT(ns ==
				    expect[(i + 1) - IB_IDLE_STREAK_LIMIT - 1]);
	}

	TEST_ASSERT(s.shift == IB_LONG_MAX_SHIFT);

	/* Further ticks stay at the cap. */
	TEST_ASSERT(ib_tick(&s, T0 + 3000000ULL + 13000, 0) == IB_LONG_MAX_NS);
	TEST_ASSERT(s.shift == IB_LONG_MAX_SHIFT);

	return 0;
}

/*
 * Work resets the streak and the growth. The next dormant phase
 * starts again from the base sleep.
 */
static int test_work_resets_state(void)
{
	struct ib_state s;
	uint64_t now = T0;
	int i;

	ib_init(&s, T0);

	/*
	 * Grow into the dormant regime: ten ticks 1 ms apart. The
	 * last two past the streak limit set the shift to 1 and 2.
	 */
	for (i = 0; i < 10; i++) {
		now += 1000000;
		(void)ib_tick(&s, now, 0);
	}
	TEST_ASSERT(s.shift == 2);

	/* One work event resets everything. */
	now += 1000000;
	TEST_ASSERT(ib_tick(&s, now, 1) == 0);
	TEST_ASSERT(s.streak == 0);
	TEST_ASSERT(s.shift == 0);

	/*
	 * Idle again for 3 ms, then cross the streak limit: the
	 * dormant growth must restart from the base, not resume
	 * where it left off.
	 */
	now += 3000000;
	for (i = 0; i < (int)IB_IDLE_STREAK_LIMIT + 1; i++) {
		now += 1000;
		uint64_t ns = ib_tick(&s, now, 0);

		if ((uint32_t)(i + 1) < (int)IB_IDLE_STREAK_LIMIT + 1)
			TEST_ASSERT(ns == 0);
		else
			TEST_ASSERT(ns == IB_LONG_BASE_NS);
	}

	return 0;
}

/*
 * A work event inside the no-sleep window restarts the streak, so
 * no sleep is due right after it.
 */
static int test_work_inside_streak_window(void)
{
	struct ib_state s;
	uint64_t now = T0;
	int i;

	ib_init(&s, T0);

	for (i = 0; i < 5; i++) {
		now += 1000;
		TEST_ASSERT(ib_tick(&s, now, 0) == 0);
	}

	now += 1000;
	TEST_ASSERT(ib_tick(&s, now, 1) == 0);

	for (i = 0; i < (int)IB_IDLE_STREAK_LIMIT; i++) {
		now += 1000;
		TEST_ASSERT(ib_tick(&s, now, 0) == 0);
	}

	return 0;
}

/* The cap holds across a long run of no-work ticks. */
static int test_cap_holds_over_time(void)
{
	struct ib_state s;
	uint64_t now = T0;
	int i;

	ib_init(&s, T0);

	now += 3000000;

	for (i = 0; i < 1000; i++) {
		uint64_t ns;

		now += 1000;
		ns = ib_tick(&s, now, 0);

		if ((uint32_t)(i + 1) > IB_IDLE_STREAK_LIMIT)
			TEST_ASSERT(ns <= IB_LONG_MAX_NS);
	}

	return 0;
}

int main(void)
{
	SUBTEST(test_init_no_sleep);
	SUBTEST(test_active_streak_window);
	SUBTEST(test_active_regime_stays_short);
	SUBTEST(test_dormant_growth_and_cap);
	SUBTEST(test_work_resets_state);
	SUBTEST(test_work_inside_streak_window);
	SUBTEST(test_cap_holds_over_time);

	printf("%s: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);

	return g_failures ? 1 : 0;
}
