/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef __Unikraft__

#include <time.h>

/* -------------------------------------------------------------------------
 * Host Test Suite Implementations (Mock DMA and Logging)
 * ------------------------------------------------------------------------- */

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
	void *virt = NULL;
	int ret = posix_memalign(&virt, 4096, size);
	if (ret != 0 || !virt)
		return NULL;

	memset(virt, 0, size);

	if (phys_out)
		*phys_out = (uint64_t)(uintptr_t)virt;

	return virt;
}

void ena_dma_free(void *virt, uint64_t phys)
{
	(void)phys;
	free(virt);
}

void ena_delay_us(unsigned int us)
{
	struct timespec ts;
	ts.tv_sec = us / 1000000;
	ts.tv_nsec = (long)(us % 1000000) * 1000;
	nanosleep(&ts, NULL);
}

static void ena_log_emit(FILE *stream, const char *prefix, const char *fmt, va_list args)
{
	fprintf(stream, "%s ", prefix);
	vfprintf(stream, fmt, args);
	fprintf(stream, "\n");
}

void ena_info(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ena_log_emit(stdout, "[INFO] ena:", fmt, args);
	va_end(args);
}

void ena_warn(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ena_log_emit(stderr, "[WARN] ena:", fmt, args);
	va_end(args);
}

void ena_err(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ena_log_emit(stderr, "[ERR]  ena:", fmt, args);
	va_end(args);
}

void ena_debug(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ena_log_emit(stdout, "[DBG]  ena:", fmt, args);
	va_end(args);
}

#else /* __Unikraft__ */

#include <uk/alloc.h>
#include <uk/plat/memory.h>
#include <uk/plat/time.h>

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
	/* Reserve low memory (< 1MB) so all heap allocations are DMA-safe */
	while (1) {
		void *p = uk_malloc(uk_alloc_get_default(), 4096);
		if (!p)
			break;
		if ((uintptr_t)p >= 0x100000ULL) {
			uk_free(uk_alloc_get_default(), p);
			break;
		}
	}

	void *virt = uk_memalign(uk_alloc_get_default(), 4096, size);
	if (!virt)
		return NULL;

	memset(virt, 0, size);

	if (phys_out)
		*phys_out = (uint64_t)(uintptr_t)virt;

	return virt;
}

void ena_dma_free(void *virt, uint64_t phys)
{
	(void)phys;
	uk_free(uk_alloc_get_default(), virt);
}

void ena_delay_us(unsigned int us)
{
	__nsec deadline = ukplat_monotonic_clock() + ((__nsec)us * 1000ULL);
	while (ukplat_monotonic_clock() < deadline) {
		__asm__ __volatile__("pause");
	}
}

#endif /* __Unikraft__ */
