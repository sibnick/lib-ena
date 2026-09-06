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
 * httpreply-mc: multi-core HTTP reply benchmark server.
 *
 * Worker thread framework (ticket 126efe1e43). Threaded lwIP (one
 * dispatcher thread per RX queue) and per-core worker threads on a
 * 2-vCPU KVM target with the ENA driver in 2-queue-pair mode. The
 * main thread runs as worker 0 and keeps its own connections, so its
 * ring is NULL. Every other worker is a uk_thread pinned to its own
 * vCPU, fed by a lock-free SPSC ring of accepted file descriptors.
 * The accept / distribute loop is added in ticket 063257c57d, and
 * the HTTP request parsing and response in ticket 2500f30961.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread.h>

#include "spsc.h"

#define MC_MAX_WORKERS 2
#define MC_RECVBUF_SIZE 8192

struct worker_ctx {
	int worker_id;
	int epoll_fd;
	struct spsc_ring *ring;
	int conn_count;
	unsigned char *recv_buf;
};

static struct spsc_ring mc_rings[MC_MAX_WORKERS];
static struct worker_ctx mc_workers[MC_MAX_WORKERS];

/*
 * Get the scheduler that owns vCPU idx by walking uk_sched_head,
 * the linked list of all per-LCPU schedulers. Index 0 is the BSP
 * (vCPU 0) and index 1 is vCPU 1. Returns NULL when the list is
 * shorter than idx + 1.
 */
static struct uk_sched *mc_get_sched(int idx)
{
	struct uk_sched *s;
	int i;

	s = uk_sched_head;
	for (i = 0; i < idx; i++) {
		if (s == NULL)
			return NULL;
		s = s->next;
	}

	return s;
}

/*
 * Per-worker loop. A worker with a ring drains it first: every fd
 * pushed by the accept loop is registered for read on the worker's
 * own epoll instance. The worker then polls its epoll set and
 * yields, so an idle worker does not busy-wait its core.
 */
static __noreturn void worker_loop(void *arg)
{
	struct worker_ctx *w = (struct worker_ctx *)arg;
	struct epoll_event evs[64];
	int fd;
	int n;
	int i;

	for(;;) {
		if (w->ring != NULL) {
			while (spsc_ring_pop(w->ring, &fd)) {
				struct epoll_event ev;

				ev.events = EPOLLIN;
				ev.data.fd = fd;
				epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
				w->conn_count++;
			}
		}

		n = epoll_wait(w->epoll_fd, evs, 64, 1);
		if (n > 0) {
			for (i = 0; i < n; i++) {
				/* HTTP request parsing and response are added in ticket 2500f30961. */
				(void)evs[i];
			}
		}

		uk_sched_yield();
	}
}

int main(int argc, char **argv)
{
	int nworkers = 0;
	int i;
	struct uk_sched *s;

	(void)argc;
	(void)argv;

	printf("httpreply-mc: multi-core HTTP reply benchmark server\n");
	printf("httpreply-mc: mode=multi-core (threaded lwIP, 2 vCPU)\n");

	for (s = uk_sched_head; s != NULL; s = s->next)
		nworkers++;

	if (nworkers < 1)
		nworkers = 1;
	if (nworkers > MC_MAX_WORKERS)
		nworkers = MC_MAX_WORKERS;

	printf("httpreply-mc: workers=%d\n", nworkers);

	for (i = 0; i < nworkers; i++) {
		mc_workers[i].worker_id = i;
		mc_workers[i].epoll_fd = epoll_create(1);
		mc_workers[i].recv_buf = malloc(MC_RECVBUF_SIZE);
		mc_workers[i].conn_count = 0;
		spsc_ring_init(&mc_rings[i]);
		/* Worker 0 is the main thread: it keeps its own
		 * connections, so it has no ring. */
		mc_workers[i].ring = (i == 0) ? NULL : &mc_rings[i];
	}

	for (i = 1; i < nworkers; i++) {
		struct uk_sched *ws = mc_get_sched(i);
		struct uk_thread *th;

		/* Graceful single-core degradation: when the list of
		 * schedulers is too short, run the worker on the
		 * current scheduler. */
		if (ws == NULL)
			ws = uk_sched_current();

		th = uk_sched_thread_create(ws, worker_loop, &mc_workers[i], "worker");
		if (th == NULL) {
			printf("httpreply-mc: [ERR] failed to create worker %d\n", i);
			return 1;
		}
	}

	worker_loop(&mc_workers[0]);

	return 0;
}
