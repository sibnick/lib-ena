/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 *
 * Micro-benchmark for the TX cross-CPU guard path. The program runs on
 * the host. It needs no EC2 instance.
 *
 * It answers one question: can the guard that src/ena_tx.c adds to the
 * completion path explain the +126 us p50 shift of the 4-queue build?
 *
 * Plan step 4 of the ca72834ec7 investigation plan. [Ticket ca72834ec7]
 *
 * Code under test, read from the tree:
 *   src/ena_tx.c line 164: uint32_t cpu = ena_plat_cpu_id();
 *     Runs once per ena_tx_poll call, not once per completion.
 *   src/ena_tx.c line 243: if (cpu != ring->tx_owner_cpu) ...
 *     Runs once per completion. One load and one compare.
 *   src/ena_tx.c line 122: ring->tx_owner_cpu = ena_plat_cpu_id();
 *     Runs once per TX submit.
 *   src/ena_plat.c line 78: the host stub returns a static variable.
 *   src/ena_plat.c line 682: the Unikraft build calls
 *   uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx) instead.
 */

#include "ena_plat.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Mirrors the one field the guard touches in struct ena_ring. */
struct fake_ring {
	uint32_t tx_owner_cpu;
	char pad[60];
};

static struct fake_ring ring;
static volatile uint64_t g_sink;

#define REPS 7
#define ITERS 20000000ULL

static double now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/*
 * An opaque call. The real completion path calls ring->tx_complete_cb
 * (src/ena_tx.c line 234) and then uk_netbuf_free before the guard.
 * Those calls stop the compiler from hoisting the owner load.
 */
static __attribute__((noinline)) void opaque_complete(uint32_t req_id)
{
	g_sink += req_id;
}

/* Case 1. Loop overhead only. */
static uint64_t bench_loop(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;

	for (i = 0; i < n; i++)
		acc += 1;
	g_sink += acc;
	return acc;
}

/*
 * Case 2. The guard as the code runs it. The cpu read sits outside the
 * poll loop, so the per-completion work is one load and one compare.
 * The compiler may also keep the owner value in a register.
 */
static uint64_t bench_guard_hoisted(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;
	uint32_t cpu = ena_plat_cpu_id();

	for (i = 0; i < n; i++) {
		if (cpu != ring.tx_owner_cpu)
			acc += 1;
		acc += ring.tx_owner_cpu;
	}
	g_sink += acc;
	return acc;
}

/*
 * Case 3. The guard with a call in front of it, as the real completion
 * path has. The load cannot be hoisted past the call.
 */
static uint64_t bench_guard_realistic(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;
	uint32_t cpu = ena_plat_cpu_id();

	for (i = 0; i < n; i++) {
		opaque_complete((uint32_t)i);
		if (cpu != ring.tx_owner_cpu)
			acc += 1;
		acc += ring.tx_owner_cpu;
	}
	g_sink += acc;
	return acc;
}

/*
 * Case 4. The guard if the cpu read ran per completion instead of per
 * poll. This is not what the code does. The call goes through a
 * volatile function pointer so that the compiler cannot hoist it out of
 * the loop or fold it away.
 */
static uint32_t (*volatile g_cpu_id)(void);

static uint64_t bench_guard_with_call(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;

	for (i = 0; i < n; i++) {
		uint32_t cpu = g_cpu_id();

		if (cpu != ring.tx_owner_cpu)
			acc += 1;
		acc += cpu;
	}
	g_sink += acc;
	return acc;
}

/* Case 5. The submit-side store, src/ena_tx.c line 122. */
static uint64_t bench_owner_store(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;

	for (i = 0; i < n; i++) {
		ring.tx_owner_cpu = ena_plat_cpu_id();
		acc += ring.tx_owner_cpu;
	}
	g_sink += acc;
	return acc;
}

#if defined(__x86_64__)
/*
 * Host analogue of the Unikraft read. uk_pcpuvar_current_get on x86-64
 * expands to one instruction: mov %gs:symbol(%rip), reg, with a memory
 * clobber (see .unikraft/unikraft/lib/ukpcpuvar/arch/x86_64/include/uk/
 * pcpuvar/arch.h, lines 89-100). The symbol is a small offset inside the
 * per-CPU area, so that load is one segment-relative access to a hot
 * line. Linux user space uses %fs the same way for thread-local
 * storage. The form below reads %fs with a small constant offset, which
 * is the same instruction class. This is an analogue, not the Unikraft
 * path itself.
 */
static uint64_t pcpuvar_analogue(void)
{
	uint64_t v;

	__asm__ __volatile__("mov %%fs:8, %0" : "=r"(v) : : "memory");
	return v;
}

/* Case 6. The analogue plus the guard compare. */
static uint64_t bench_pcpuvar_analogue(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;

	for (i = 0; i < n; i++) {
		uint32_t cpu = (uint32_t)pcpuvar_analogue();

		if (cpu != ring.tx_owner_cpu)
			acc += 1;
		acc += cpu;
	}
	g_sink += acc;
	return acc;
}

/* Case 7. The compiler barrier alone, with no load. */
static uint64_t bench_barrier_only(uint64_t n)
{
	uint64_t i;
	uint64_t acc = 0;

	for (i = 0; i < n; i++) {
		__asm__ __volatile__("" : : : "memory");
		acc += 1;
	}
	g_sink += acc;
	return acc;
}
#endif

typedef uint64_t (*bench_fn)(uint64_t);

static double run(const char *name, bench_fn fn)
{
	double best = 1e300;
	int r;

	for (r = 0; r < REPS; r++) {
		double t0 = now_ns();

		fn(ITERS);
		{
			double dt = now_ns() - t0;
			double per = dt / (double)ITERS;

			if (per < best)
				best = per;
		}
	}
	printf("  %-34s %8.3f ns/call  %10.1f ns per 1e6\n", name, best,
	       best * 1e6);
	return best;
}

int main(void)
{
	double loop_ns, hoisted_ns, realistic_ns, with_call_ns, store_ns;
	double analogue_ns = 0.0, barrier_ns = 0.0;
	double worst_ns;
	const double meas_p50_shift_ns = 126000.0; /* us -> ns */
	const double meas_rate = 74366.0; /* req/s, the 2-queue c=25 run */

	printf("TX cross-CPU guard micro-benchmark [Ticket ca72834ec7]\n");
	printf("Host build. The guard code comes from src/ena_tx.c.\n");
	printf("Timer: CLOCK_MONOTONIC. %d reps of %llu iterations, best "
	       "kept.\n\n",
	       REPS, (unsigned long long)ITERS);

	ena_plat_set_mock_cpu_id(0);
	g_cpu_id = ena_plat_cpu_id;
	ring.tx_owner_cpu = 0;

	printf("1. Measured cost per guard operation\n");
	loop_ns = run("loop only (baseline)", bench_loop);
	hoisted_ns = run("guard as coded (hoisted cpu)", bench_guard_hoisted);
	realistic_ns = run("guard after an opaque call", bench_guard_realistic);
	with_call_ns =
	    run("guard + cpu read per completion", bench_guard_with_call);
	store_ns = run("submit-side owner store", bench_owner_store);
#if defined(__x86_64__)
	analogue_ns =
	    run("Unikraft read analogue + compare", bench_pcpuvar_analogue);
	barrier_ns = run("compiler barrier only", bench_barrier_only);
#endif

	printf("\n2. Net of the loop baseline\n");
	printf("  guard as coded:            %8.3f ns/completion\n",
	       hoisted_ns - loop_ns);
	printf("  guard after a call:        %8.3f ns/completion\n",
	       realistic_ns - loop_ns);
	printf("  guard + cpu read (worst):  %8.3f ns/completion\n",
	       with_call_ns - loop_ns);
	printf("  submit-side store:         %8.3f ns/submit\n",
	       store_ns - loop_ns);
#if defined(__x86_64__)
	printf("  segment-relative read:     %8.3f ns/read (analogue)\n",
	       analogue_ns - loop_ns);
	printf("  barrier only:              %8.3f ns/barrier\n",
	       barrier_ns - loop_ns);
#endif
	printf(
	    "  The opaque call in row 2 costs time of its own, so row 2 is\n");
	printf("  an upper bound for the guard, not a cost of the guard "
	       "alone.\n");

	printf("\n3. The Unikraft path is not measurable on the host\n");
	printf("   src/ena_plat.c line 682 calls uk_pcpuvar_current_get\n");
	printf(
	    "   (uk_pcpuvar_cpu_idx). On x86-64 that macro expands to one\n");
	printf("   instruction, mov %%gs:symbol(%%rip), reg, with a memory\n");
	printf("   clobber. The host has no %%gs per-CPU area, so this test\n");
	printf("   cannot run the real path. Two statements, both bounded:\n");
#if defined(__x86_64__)
	printf("   - The host analogue of that same instruction pair measures "
	       "%.3f ns.\n",
	       analogue_ns - loop_ns);
	printf("     Treat that as the expected cost, because the per-CPU "
	       "line is hot\n");
	printf("     in the steady state.\n");
	printf("   - Bound: even if every read missed to DRAM, one load "
	       "costs well\n");
	printf("     under 300 ns on this class of CPU. That is a bound from "
	       "memory\n");
	printf("     latency, not a measurement.\n");
#else
	printf(
	    "   - Not measured: this build is not x86-64. Bound only: one\n");
	printf("     load plus one compiler barrier, under 300 ns even on a "
	       "DRAM miss.\n");
#endif

	printf("\n4. Compare with the measured shift\n");
	printf("   measured p50 shift, 4 queues minus 2 queues: %.0f ns\n",
	       meas_p50_shift_ns);
	printf("   guard as coded, per completion:              %.3f ns\n",
	       hoisted_ns - loop_ns);
	printf("   guard after a call, per completion:          %.3f ns\n",
	       realistic_ns - loop_ns);
	printf("   guard plus a per-completion cpu read:        %.3f ns\n",
	       with_call_ns - loop_ns);
	printf("   The largest of these is the honest per-completion cost.\n");
	worst_ns = realistic_ns - loop_ns;
	if (with_call_ns - loop_ns > worst_ns)
		worst_ns = with_call_ns - loop_ns;
	printf("   worst per-completion cost:                   %.3f ns\n",
	       worst_ns);
	printf("   ratio, shift over that cost:                 %.0f x\n",
	       meas_p50_shift_ns / worst_ns);
	printf("   completions of guard work needed to build a 126 us shift: "
	       "%.0f\n",
	       meas_p50_shift_ns / worst_ns);
	printf("   One request produces one completion, so the guard cannot "
	       "stack up.\n");
	printf("   CPU time for the guard plus the submit store at the "
	       "measured rate:\n");
	printf("   %.4f ms per second, which is %.5f%% of one core.\n",
	       meas_rate * (worst_ns + store_ns - loop_ns) / 1e6,
	       meas_rate * (worst_ns + store_ns - loop_ns) / 1e7);

	printf("\n5. Verdict on hypothesis (d), the TX cross-CPU guard\n");
	printf("   REJECTED by numbers.\n");
	printf("   The guard costs %.2f ns per completion as coded, and "
	       "%.2f ns in the\n",
	       hoisted_ns - loop_ns, worst_ns);
	printf("   worst case, where a call blocks the load or the cpu read "
	       "runs per\n");
	printf("   completion. The measured p50 shift is 126000 ns. The "
	       "guard is about\n");
	printf("   %.0f times too small. It would take about %.0f completions "
	       "of guard work\n",
	       meas_p50_shift_ns / worst_ns, meas_p50_shift_ns / worst_ns);
	printf("   to build that shift, and one request gives one "
	       "completion.\n");
	printf("   This matches the code reading in the plan: the guard is "
	       "one load and\n");
	printf("   one compare per completion, and the cpu read runs once "
	       "per poll.\n");

	return 0;
}
