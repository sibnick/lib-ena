/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena_plat.h"

#include <stdint.h>

#ifndef __Unikraft__

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
	void *virt = NULL;
	int ret;

	ret = posix_memalign(&virt, 4096, size);
	if (ret != 0 || !virt)
		return NULL;

	memset(virt, 0, size);

	/* Host build uses identity mapping: phys == virt. */
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
	/* Host build: a light spin keeps bounded poll loops fast. */
	volatile unsigned int i;

	for (i = 0; i < us; i++)
		;
}

static void ena_log_emit(FILE *stream, const char *tag,
		const char *fmt, va_list args)
{
	fprintf(stream, "%s ", tag);
	vfprintf(stream, fmt, args);
	fputc('\n', stream);
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
	ena_log_emit(stdout, "[WARN] ena:", fmt, args);
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
#include <uk/plat.h>

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
	void *virt = uk_memalign(uk_alloc_get_default(), 4096, size);

	if (!virt)
		return NULL;

	memset(virt, 0, size);

	/* Translate the virtual address to the physical (bus) address that
	 * the device uses for DMA. */
	if (phys_out)
		*phys_out = ukplat_virt_to_phys(virt);

	return virt;
}

void ena_dma_free(void *virt, uint64_t phys)
{
	(void)phys;
	uk_free(uk_alloc_get_default(), virt);
}

void ena_delay_us(unsigned int us)
{
	/* Use the platform delay routine for a calibrated microsecond delay. */
	ukplat_time_delay_us(us);
}

#endif /* __Unikraft__ */
