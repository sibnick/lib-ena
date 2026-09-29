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
#include <uk/intctlr.h>
#include <uk/intctlr/msix.h>
#include <uk/plat/memory.h>
#include <uk/plat/time.h>
#include <uk/arch/util.h>

/* PCI config space access (same method as the probe path in ena_pci.c). */
static uint32_t plat_pci_cfg_read(const struct pci_address *addr,
				      uint32_t reg)
{
	uint32_t config_addr = (1u << 31)
		| ((uint32_t)addr->bus << 16)
		| ((uint32_t)addr->devid << 11)
		| ((uint32_t)addr->function << 8)
		| (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

static void plat_pci_cfg_write(const struct pci_address *addr,
				     uint32_t reg, uint32_t val)
{
	uint32_t config_addr = (1u << 31)
		| ((uint32_t)addr->bus << 16)
		| ((uint32_t)addr->devid << 11)
		| ((uint32_t)addr->function << 8)
		| (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	uk_arch_x86_64_outl(PCI_CONFIG_DATA, val);
}

/* PCI capability ID for MSI-X (PCI revision 3.x). */
#define ENA_PLAT_PCI_CAP_ID_MSIX	11u

/*
 * Probe the device MSI-X capability and report the number of
 * vectors it exposes (message control count + 1). A count of
 * zero means the driver must stay in software polling mode.
 */
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

			if (msg_ctrl & 0x0001u) {
				ena_info("msix: capability is masked");
				return 0;
			}

			/* The count field encodes vectors minus one. */
			uint32_t count = (msg_ctrl >> 1) & 0x7FFFu;
			uint32_t nvec = 1;

			while (nvec <= count)
				nvec <<= 1;
			*num_vectors = nvec;
			ena_info("msix: device exposes %u vectors", (unsigned)nvec);
			return 0;
		}

		cap = next;
	}

	ena_info("msix: no MSI-X capability found");
	return 0;
}


/*
 * MSI-X table and PBA location decoded from the capability. The
 * four low bits of each 32-bit offset select the BAR; the rest
 * is the byte offset inside that BAR. The PBA lives in the same
 * BAR as the table.
 */
struct ena_msix_loc {
	const struct pci_address *pci_dev;
	uint32_t msgctl_off;
	uint32_t count;
	void *table;
	void *pba;
};

static int msix_find(const struct pci_address *addr, struct ena_msix_loc *loc)
{
	uint32_t cap;
	uint32_t bar_reg;
	uint32_t bar_lo;
	uint64_t bar_base;

	memset(loc, 0, sizeof(*loc));
	loc->pci_dev = addr;

	cap = plat_pci_cfg_read(addr, 0x34) & 0xFCu;
	while (cap) {
		uint32_t cap_id = plat_pci_cfg_read(addr, cap) & 0xFFu;
		uint32_t next = plat_pci_cfg_read(addr, cap + 1) & 0xFFu;

		if (cap_id == ENA_PLAT_PCI_CAP_ID_MSIX) {
			uint32_t msg_ctrl = plat_pci_cfg_read(addr, cap + 2);
			uint32_t table_off = plat_pci_cfg_read(addr, cap + 8);
			uint32_t pba_off = plat_pci_cfg_read(addr, cap + 12);
			uint32_t bar;

			if (msg_ctrl & 0x0001u)
				return -EAGAIN; /* masked: never arm */

			loc->msgctl_off = cap + 2;
			loc->count = ((msg_ctrl >> 1) & 0x7FFFu) + 1;

			/*
			 * The KVM and QEMU platforms map guest physical
			 * addresses 1:1, the same assumption the BAR mapping
			 * in ena_pci.c uses. The BAR value read from config
			 * space is directly usable as a virtual address.
			 */
			bar = table_off & 0xFu;
			bar_reg = 0x10 + 4 * bar;
			bar_lo = plat_pci_cfg_read(addr, bar_reg) & ~0x0Fu;
			if ((plat_pci_cfg_read(addr, bar_reg) & 0x06u) == 0x04u) {
				/* 64-bit memory BAR: the high part is the next dword. */
				bar_base = (uint64_t)bar_lo
					| ((uint64_t)plat_pci_cfg_read(addr, bar_reg + 4) << 32);
			} else {
				bar_base = bar_lo;
			}

			if (bar_base == 0)
				return -ENODEV;

			loc->table = (void *)(bar_base + (table_off & ~0xFu));
			loc->pba = (void *)(bar_base + (pba_off & ~0xFu));
			return 0;
		}

		cap = next;
	}

	return -ENODEV;
}


/* One trampoline argument per armed vector. */
struct msix_tramp_ctx {
	uint32_t vector_id;
};

/*
 * State armed by ena_plat_msix_arm(). One trampoline context is
 * stored per allocated vector, plus the driver request and the
 * decoded table and PBA location.
 */
static struct {
	int armed;
	uint32_t nvec;
	unsigned int irqs[ENA_PLAT_MSIX_MAX_VECTORS];
	uint32_t count[ENA_PLAT_MSIX_MAX_VECTORS];
	struct msix_tramp_ctx ctx[ENA_PLAT_MSIX_MAX_VECTORS];
	struct ena_msix_req req;
	struct ena_msix_loc loc;
} s_msix;

/*
 * The EOI for the delivered vector is written by the xpic handler
 * after the event dispatch, so this trampoline only counts the
 * delivery and hands it to the driver callback.
 */
static int msix_tramp(void *arg)
{
	struct msix_tramp_ctx *ctx = arg;

	s_msix.count[ctx->vector_id]++;
	if (s_msix.req.on_fire)
		s_msix.req.on_fire(s_msix.req.arg, ctx->vector_id);
	return 0;
}

/*
 * Arm the device MSI-X: allocate one unikernel interrupt vector
 * per entry, program the 16-byte table entries and clear the PBA
 * bits, then unmask the capability.
 */
int ena_plat_msix_arm(const struct ena_msix_req *req)
{
	volatile uint32_t *table;
	volatile uint32_t *pba;
	int nvec;
	int i;
	int ret;

	if (!req || !req->pci_dev)
		return -EINVAL;

	if (s_msix.armed)
		return -EBUSY;

	ret = msix_find(req->pci_dev, &s_msix.loc);
	if (ret) {
		ena_warn("msix: capability lookup failed (%d)", ret);
		return ret;
	}

	table = (volatile uint32_t *)s_msix.loc.table;
	pba = (volatile uint32_t *)s_msix.loc.pba;

	nvec = (int)req->nvec;
	if (nvec > (int)s_msix.loc.count)
		nvec = (int)s_msix.loc.count;
	if (nvec > ENA_PLAT_MSIX_MAX_VECTORS)
		nvec = ENA_PLAT_MSIX_MAX_VECTORS;
	if (nvec <= 0)
		return -ENODEV;

	s_msix.req = *req;
	s_msix.req.nvec = (uint32_t)nvec;

	for (i = 0; i < nvec; i++) {
		__u64 maddr;
		__u32 mdata;

		ret = uk_intctlr_msix_alloc(req->lcpu[i],
						 &s_msix.irqs[i],
						 &maddr, &mdata);
		if (ret) {
			ena_err("msix: vector %d allocation failed (%d)",
				      i, ret);
			goto err_free;
		}

		s_msix.ctx[i].vector_id = (uint32_t)i;

		/* Program the 16-byte table entry: address, data, reserved. */
		table[4 * i + 0] = (uint32_t)(maddr & 0xFFFFFFFFu);
		table[4 * i + 1] = (uint32_t)(maddr >> 32);
		table[4 * i + 2] = mdata;
		table[4 * i + 3] = 0;

		/* Unmask the vector: clear its PBA bit. */
		pba[i / 32] &= ~(1u << (i % 32));

		ret = uk_intctlr_irq_register(s_msix.irqs[i], msix_tramp,
					&s_msix.ctx[i]);
		if (ret) {
			ena_err("msix: handler %d register failed (%d)", i, ret);
			goto err_free;
		}
	}

	{
		uint32_t msg_ctrl = plat_pci_cfg_read(s_msix.loc.pci_dev,
						    s_msix.loc.msgctl_off);

		/* Enable the capability: clear the masked bit. */
		plat_pci_cfg_write(s_msix.loc.pci_dev, s_msix.loc.msgctl_off,
			       msg_ctrl & ~0x1u);
	}

	s_msix.armed = 1;
	s_msix.nvec = (uint32_t)nvec;
	ena_info("msix: armed %d vectors", nvec);
	return 0;

err_free:
	for (i--; i >= 0; i--) {
		uk_intctlr_irq_unregister(s_msix.irqs[i], msix_tramp);
		uk_intctlr_msix_free(s_msix.irqs[i]);
	}
	return ret;
}


/*
 * Release all armed vectors and disable the capability. Safe to
 * call when not armed (no-op).
 */
void ena_plat_msix_disarm(void)
{
	volatile uint32_t *pba;
	uint32_t i;
	uint32_t msg_ctrl;

	if (!s_msix.armed)
		return;

	pba = (volatile uint32_t *)s_msix.loc.pba;
	for (i = 0; i < s_msix.nvec; i++) {
		/* Re-mask the vector in the PBA, then release it. */
		pba[i / 32] |= 1u << (i % 32);
		uk_intctlr_irq_unregister(s_msix.irqs[i], msix_tramp);
		uk_intctlr_msix_free(s_msix.irqs[i]);
	}

	msg_ctrl = plat_pci_cfg_read(s_msix.loc.pci_dev,
				    s_msix.loc.msgctl_off);
	msg_ctrl |= 0x1u; /* Disable the capability: set the masked bit. */
	plat_pci_cfg_write(s_msix.loc.pci_dev, s_msix.loc.msgctl_off,
		       msg_ctrl);
	s_msix.armed = 0;
	s_msix.nvec = 0;
	ena_info("msix: disarmed");
}

/* Total MSI-X interrupts delivered since arm. */
uint32_t ena_plat_msix_count_get(void)
{
	uint32_t total = 0;
	uint32_t i;

	for (i = 0; i < s_msix.nvec; i++)
		total += s_msix.count[i];
	return total;
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
		ena_pause();
	}
}

#endif /* __Unikraft__ */
