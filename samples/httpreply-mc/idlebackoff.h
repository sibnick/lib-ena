/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Idle backoff policy for the httpreply-mc run-to-completion worker loop.
 *
 * The worker loop once burned 100% of both vCPUs at all loads, including
 * zero traffic: at idle it ran 1.3-2.3 M no-work loop iterations per
 * second, and each iteration paid a fixed cost (a TSC read, an ARP walk,
 * an epoll lock, hardware queue peeks). [Ticket 597c1b9731]
 *
 * This policy decides, per core and per loop iteration, whether the core
 * may sleep and for how long. It is pure: it takes a monotonic nanosecond
 * time and a work flag, and returns a sleep duration in nanoseconds. The
 * caller performs the actual sleep (a real vCPU halt on the KVM platform).
 *
 * Two regimes:
 *
 * Active: work was seen within the last IB_ACTIVE_WINDOW_NS. After
 * IB_IDLE_STREAK_LIMIT consecutive no-work iterations, the core takes a
 * short fixed sleep (IB_ACTIVE_SLEEP_NS). This bounds the added wake
 * latency during traffic.
 *
 * Dormant: no work for IB_ACTIVE_WINDOW_NS or more. After the streak
 * limit, the sleep budget grows exponentially from IB_LONG_BASE_NS to
 * IB_LONG_MAX_NS and stays there. At zero load the core then spends
 * nearly all of its time halted, so both vCPUs idle. The first request
 * after a dormant period waits at most IB_LONG_MAX_NS to wake a core.
 */
#ifndef __HTTPREPLY_MC_IDLEBACKOFF_H__
#define __HTTPREPLY_MC_IDLEBACKOFF_H__

#include <stdint.h>

/*
 * Consecutive no-work iterations before a sleep starts. Eight
 * iterations cost about 6 us, the measured no-work iteration time.
 */
#define IB_IDLE_STREAK_LIMIT	8U

/* Work seen within this window keeps the core in the active regime. */
#define IB_ACTIVE_WINDOW_NS	2000000ULL	/* 2 ms */

/*
 * Active-regime sleep. 20 us sits just above the KVM guest's PIT
 * one-shot floor (16 ticks, about 13.4 us), so it is a real halt,
 * not a spin.
 */
#define IB_ACTIVE_SLEEP_NS	20000ULL

/* Dormant-regime sleep: IB_LONG_BASE_NS << shift, capped at the max. */
#define IB_LONG_BASE_NS		1000000ULL	/* 1 ms */
#define IB_LONG_MAX_NS		8000000ULL	/* 8 ms */
#define IB_LONG_MAX_SHIFT	3U	/* 1, 2, 4, 8 ms */

struct ib_state {
	uint32_t streak;	/* consecutive no-work iterations	*/
	uint32_t shift;	/* dormant-regime sleep exponent	*/
	uint64_t last_work;	/* monotonic ns of the last work	*/
};

static inline void
ib_init(struct ib_state *s, uint64_t now)
{
	s->streak = 0;
	s->shift = 0;
	s->last_work = now;
}

/*
 * Call once per worker-loop iteration, after the loop has polled the
 * queues and processed its events.
 *
 * now:	current monotonic time, ns.
 * work:	nonzero if this iteration found work: an epoll event, or a
 *		packet received or transmitted on this core's hardware queue.
 *
 * Returns the sleep duration in ns. Zero means no sleep.
 */
static inline uint64_t
ib_tick(struct ib_state *s, uint64_t now, int work)
{
	if (work) {
		s->streak = 0;
		s->shift = 0;
		s->last_work = now;
		return 0;
	}

	s->streak++;

	if (s->streak <= IB_IDLE_STREAK_LIMIT)
		return 0;

	if (now - s->last_work <= IB_ACTIVE_WINDOW_NS)
		return IB_ACTIVE_SLEEP_NS;

	{
		uint64_t budget = IB_LONG_BASE_NS << s->shift;

		if (budget > IB_LONG_MAX_NS)
			budget = IB_LONG_MAX_NS;

		if (s->shift < IB_LONG_MAX_SHIFT)
			s->shift++;

		return budget;
	}
}

#endif /* __HTTPREPLY_MC_IDLEBACKOFF_H__ */
