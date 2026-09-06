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
 * Lock-free single-producer, single-consumer (SPSC) ring buffer for
 * file descriptors. One producer thread (the accept loop on vCPU 0)
 * pushes accepted fds; one consumer thread (a worker on another vCPU)
 * pops them. Exactly one producer and one consumer per ring. No locks;
 * ordering is provided by acquire/release atomics on head/tail.
 */
#ifndef __HTTPREPLY_MC_SPSC_H__
#define __HTTPREPLY_MC_SPSC_H__

#include <stddef.h>
#include <stdbool.h>

#define SPSC_CAP 1024

struct spsc_ring {
	/* Items produced (write index). Written by the producer only. */
	size_t head;
	/* Items consumed (read index). Written by the consumer only. */
	size_t tail;
	/* Static slot storage holding file descriptors. */
	int data[SPSC_CAP];
};

static inline void spsc_ring_init(struct spsc_ring *r)
{
	r->head = 0;
	r->tail = 0;
}

static inline bool spsc_ring_empty(const struct spsc_ring *r)
{
	return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) ==
	       __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline bool spsc_ring_full(const struct spsc_ring *r)
{
	size_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
	size_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

	return (h - t) >= SPSC_CAP;
}

/* Producer only. Returns false when the ring is full. */
static inline bool spsc_ring_push(struct spsc_ring *r, int fd)
{
	size_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
	size_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

	if ((h - t) >= SPSC_CAP)
		return false;

	r->data[h % SPSC_CAP] = fd;
	/* Publish the slot, then advance head (release). */
	__atomic_store_n(&r->head, h + 1, __ATOMIC_RELEASE);
	return true;
}

/* Consumer only. Returns false when the ring is empty. */
static inline bool spsc_ring_pop(struct spsc_ring *r, int *fd_out)
{
	size_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
	size_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

	if (h == t)
		return false;

	*fd_out = r->data[t % SPSC_CAP];
	/* Consume, then free the slot (release) for the producer. */
	__atomic_store_n(&r->tail, t + 1, __ATOMIC_RELEASE);
	return true;
}

#endif /* __HTTPREPLY_MC_SPSC_H__ */
