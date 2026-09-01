/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena_plat.h"
#include "ena_datapath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

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

static uint32_t s_mock_msix_vectors = 0;

void ena_plat_set_mock_msix_vectors(uint32_t num_vectors)
{
	s_mock_msix_vectors = num_vectors;
}

int ena_plat_msix_probe(void *pci_dev, uint32_t *num_vectors)
{
	(void)pci_dev;

	if (!num_vectors)
		return -EINVAL;

	*num_vectors = s_mock_msix_vectors;
	return 0;
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
#include <uk/arch/util.h>

/* PCI config space access (same method as the probe path in ena_pci.c). */
static uint32_t plat_pci_cfg_read(const struct pci_address *addr, uint32_t reg)
{
	uint32_t config_addr = (1u << 31)
		| ((uint32_t)addr->bus << 16)
		| ((uint32_t)addr->devid << 11)
		| ((uint32_t)addr->function << 8)
		| (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

/* PCI capability ID for MSI-X (PCI revision 3.x). */
#define ENA_PLAT_PCI_CAP_ID_MSIX	11u

int ena_plat_msix_probe(void *pci_dev, uint32_t *num_vectors)
{
	const struct pci_address *addr = (const struct pci_address *)pci_dev;
	uint32_t cap;

	if (!num_vectors)
		return -EINVAL;

	*num_vectors = 0;
	if (!addr)
		return 0;

	/* Walk the PCI capability list for the MSI-X capability. */
	cap = plat_pci_cfg_read(addr, 0x34) & 0xFCu;
	while (cap) {
		uint32_t cap_id = plat_pci_cfg_read(addr, cap) & 0xFFu;
		uint32_t next = plat_pci_cfg_read(addr, cap + 1) & 0xFFu;

		if (cap_id == ENA_PLAT_PCI_CAP_ID_MSIX) {
			uint32_t msg_ctrl = plat_pci_cfg_read(addr, cap + 2);

			if (!(msg_ctrl & 0x0001u)) {
				uint32_t count = (msg_ctrl >> 1) & 0x7FFFu;
				uint32_t nvec = 1;

				/* The count field encodes vectors minus one. */
				while (nvec <= count)
					nvec <<= 1;
				ena_info("msix: device exposes %u vectors", (unsigned)nvec);
			} else {
				ena_info("msix: capability is masked");
			}

			/* The pinned Unikraft platform (>=0.17.0) exposes no
			 * interrupt allocation API. The driver cannot arm the
			 * MSI-X table, so it reports zero vectors and stays
			 * in software polling mode. */
			return 0;
		}

		cap = next;
	}

	ena_info("msix: no MSI-X capability found");
	return 0;
}

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
	/* Reserve low memory (< 1MB) so all heap allocations are DMA-safe */
	while (1) {
		void *p = uk_malloc(uk_alloc_get_default(), 4096);
		if (!p)
			break;
		if ((uintptr_t)p >= ENA_DMA_LOW_MEM_LIMIT) {
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
