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

static uint32_t s_mock_cpu_id = 0;

void ena_plat_set_mock_cpu_id(uint32_t cpu_id)
{
	s_mock_cpu_id = cpu_id;
}

uint32_t ena_plat_cpu_id(void)
{
	return s_mock_cpu_id;
}

static void ena_log_emit(FILE *stream, const char *prefix, const char *fmt,
			 va_list args)
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
#ifdef CONFIG_LIBENA_MSIX
#include <uk/intctlr/msix.h>
#endif
#if defined(CONFIG_LIBUKPCPUVAR) && CONFIG_LIBUKPCPUVAR
#include <uk/pcpuvar.h>
#endif
#include <uk/plat/memory.h>
#include <uk/plat/time.h>
#include <uk/arch/util.h>

/* PCI config space access (same method as the probe path in ena_pci.c). */
static uint32_t plat_pci_cfg_read(const struct pci_address *addr, uint32_t reg)
{
	uint32_t config_addr = (1u << 31) | ((uint32_t)addr->bus << 16) |
			       ((uint32_t)addr->devid << 11) |
			       ((uint32_t)addr->function << 8) | (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

static void plat_pci_cfg_write(const struct pci_address *addr, uint32_t reg,
			       uint32_t val)
{
	uint32_t config_addr = (1u << 31) | ((uint32_t)addr->bus << 16) |
			       ((uint32_t)addr->devid << 11) |
			       ((uint32_t)addr->function << 8) | (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	uk_arch_x86_64_outl(PCI_CONFIG_DATA, val);
}

/* PCI capability ID for MSI-X (PCI revision 3.x). */
#define ENA_PLAT_PCI_CAP_ID_MSIX 0x11u

/*
 * Print the PCI config space from 0x000 to 0x3FF, 16 bytes per
 * line. This runs when the capability list search misses the
 * MSI-X capability. It shows the real layout of the device.
 */
static void msix_cfg_dump(const struct pci_address *addr)
{
	uint32_t off;

	for (off = 0; off < 0x400u; off += 16u) {
		uint32_t v0 = plat_pci_cfg_read(addr, off);
		uint32_t v1 = plat_pci_cfg_read(addr, off + 4u);
		uint32_t v2 = plat_pci_cfg_read(addr, off + 8u);
		uint32_t v3 = plat_pci_cfg_read(addr, off + 12u);

		ena_info("cfg: %03x: %02x %02x %02x %02x %02x %02x %02x %02x "
			 "%02x %02x %02x %02x %02x %02x %02x %02x",
			 (unsigned)off, (unsigned)(v0 & 0xFFu),
			 (unsigned)((v0 >> 8) & 0xFFu),
			 (unsigned)((v0 >> 16) & 0xFFu),
			 (unsigned)((v0 >> 24) & 0xFFu), (unsigned)(v1 & 0xFFu),
			 (unsigned)((v1 >> 8) & 0xFFu),
			 (unsigned)((v1 >> 16) & 0xFFu),
			 (unsigned)((v1 >> 24) & 0xFFu), (unsigned)(v2 & 0xFFu),
			 (unsigned)((v2 >> 8) & 0xFFu),
			 (unsigned)((v2 >> 16) & 0xFFu),
			 (unsigned)((v2 >> 24) & 0xFFu), (unsigned)(v3 & 0xFFu),
			 (unsigned)((v3 >> 8) & 0xFFu),
			 (unsigned)((v3 >> 16) & 0xFFu),
			 (unsigned)((v3 >> 24) & 0xFFu));
	}
}

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
		/*
		 * One read gets the whole header. The low byte is
		 * the id, the next byte is the next pointer, and
		 * the high word is the message control.
		 */
		uint32_t dw = plat_pci_cfg_read(addr, cap);
		uint32_t cap_id = dw & 0xFFu;
		uint32_t next = (dw >> 8) & 0xFFu;
		uint32_t msg_ctrl = (dw >> 16) & 0xFFFFu;

		if (cap_id == ENA_PLAT_PCI_CAP_ID_MSIX) {
			/* Table Size field (bits 10:0) encodes vectors minus
			 * one. */
			uint32_t count = msg_ctrl & 0x7FFu;

			if (!(msg_ctrl & 0x8000u))
				ena_info(
				    "msix: capability is disabled at reset and "
				    "the arm path will enable it");

			*num_vectors = count + 1u;
			ena_info("msix: device exposes %u vectors",
				 (unsigned)(count + 1u));
			return 0;
		}

		cap = next;
	}

	ena_info("msix: no MSI-X capability found");
	msix_cfg_dump(addr);
	return 0;
}

/*
 * The arm path needs the patched interrupt controller that provides
 * uk_intctlr_msix_alloc(). That controller is not in released
 * Unikraft, so this whole block is built only when CONFIG_LIBENA_MSIX
 * is on. With the flag off the driver runs in software polling mode.
 */
#ifdef CONFIG_LIBENA_MSIX

/*
 * MSI-X table and PBA location. The driver decodes the location
 * from the capability. The four low bits of each 32-bit offset
 * select the BAR. The rest is the byte offset inside that BAR.
 * The PBA lives in the same BAR as the table.
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

	/* Walk the PCI capability list for the MSI-X capability. */
	cap = plat_pci_cfg_read(addr, 0x34) & 0xFCu;
	while (cap) {
		/*
		 * One read gets the header. Byte 0 is the id, byte 1
		 * is the next pointer, and the high word is the message
		 * control. The table address is the next dword, and
		 * the PBA address is the one after that.
		 */
		uint32_t dw = plat_pci_cfg_read(addr, cap);
		uint32_t cap_id = dw & 0xFFu;
		uint32_t next = (dw >> 8) & 0xFFu;
		uint32_t msg_ctrl = (dw >> 16) & 0xFFFFu;

		if (cap_id == ENA_PLAT_PCI_CAP_ID_MSIX) {
			uint32_t table_off = plat_pci_cfg_read(addr, cap + 4);
			uint32_t pba_off = plat_pci_cfg_read(addr, cap + 8);
			uint32_t bar;

			loc->msgctl_off = cap + 2;
			loc->count = (msg_ctrl & 0x7FFu) + 1u;

			ena_info("msix: raw table_off=0x%08x (bir=%u off=0x%x) "
				 "pba_off=0x%08x (bir=%u off=0x%x)",
				 table_off, (unsigned)(table_off & 0x7u),
				 (unsigned)(table_off & ~0x7u), pba_off,
				 (unsigned)(pba_off & 0x7u),
				 (unsigned)(pba_off & ~0x7u));

			/*
			 * The KVM and QEMU platforms map guest physical
			 * addresses 1:1, the same assumption the BAR mapping
			 * in ena_pci.c uses. The BAR value read from config
			 * space is directly usable as a virtual address.
			 */
			/* BIR is bits 2:0; the table offset is bits 31:3. */
			bar = table_off & 0x7u;
			bar_reg = 0x10 + 4 * bar;
			bar_lo = plat_pci_cfg_read(addr, bar_reg) & ~0x0Fu;
			if ((plat_pci_cfg_read(addr, bar_reg) & 0x06u) ==
			    0x04u) {
				/* 64-bit memory BAR: the high part is the next
				 * dword. */
				bar_base = (uint64_t)bar_lo |
					   ((uint64_t)plat_pci_cfg_read(
						addr, bar_reg + 4)
					    << 32);
			} else {
				bar_base = bar_lo;
			}

			if (bar_base == 0)
				return -ENODEV;

			loc->table = (void *)(bar_base + (table_off & ~0x7u));
			loc->pba = (void *)(bar_base + (pba_off & ~0x7u));
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
 * State that ena_plat_msix_arm() arms. It holds one trampoline
 * context per allocated vector, plus the driver request and the
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
 * The xpic handler writes the EOI for the delivered vector after
 * the event dispatch. This trampoline only counts the delivery
 * and hands it to the driver callback.
 */
static int msix_tramp(void *arg)
{
	struct msix_tramp_ctx *ctx = arg;

	s_msix.count[ctx->vector_id]++;
	if (s_msix.req.on_fire)
		s_msix.req.on_fire(s_msix.req.arg, ctx->vector_id);
	return 0;
}

/* Per-vector delivered-MSI count, for diagnostics. */
uint32_t ena_plat_msix_vector_count(uint32_t vector)
{
	if (vector >= ENA_PLAT_MSIX_MAX_VECTORS)
		return 0;
	return s_msix.count[vector];
}

/*
 * Arm the device MSI-X: allocate one unikernel interrupt vector
 * per entry, program the 16-byte table entries and clear the PBA
 * bits, then unmask the capability.
 */
int ena_plat_msix_arm(const struct ena_msix_req *req)
{
	volatile uint32_t *table;
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

		ret = uk_intctlr_msix_alloc(req->lcpu[i], &s_msix.irqs[i],
					    &maddr, &mdata);
		if (ret) {
			ena_err("msix: vector %d allocation failed (%d)", i,
				ret);
			goto err_free;
		}

		s_msix.ctx[i].vector_id = (uint32_t)i;

		/* Program the 16-byte table entry: address, data, reserved. */
		table[4 * i + 0] = (uint32_t)(maddr & 0xFFFFFFFFu);
		table[4 * i + 1] = (uint32_t)(maddr >> 32);
		table[4 * i + 2] = mdata;
		table[4 * i + 3] = 0;

		ret = uk_intctlr_irq_register(s_msix.irqs[i], msix_tramp,
					      &s_msix.ctx[i]);
		if (ret) {
			ena_err("msix: handler %d register failed (%d)", i,
				ret);
			uk_intctlr_msix_free(s_msix.irqs[i]);
			goto err_free;
		}
	}

	{
		/*
		 * msgctl is the high word of the dword at the capability
		 * base. The config accessor is dword-only, so read the
		 * dword, change the high word, and write it back.
		 */
		uint32_t base = s_msix.loc.msgctl_off & ~3u;
		uint32_t dw = plat_pci_cfg_read(s_msix.loc.pci_dev, base);
		uint32_t msg_ctrl = (dw >> 16) & 0xFFFFu;
		uint32_t new_ctrl;

		/* Enable MSI-X (bit 15) and clear the function mask (bit 14).
		 */
		new_ctrl = (msg_ctrl & ~0x4000u) | 0x8000u;
		plat_pci_cfg_write(s_msix.loc.pci_dev, base,
				   (dw & 0xFFFFu) | (new_ctrl << 16));

		ena_info(
		    "msix: msgctl before=0x%04x en=%u mask=%u after=0x%04x "
		    "en=%u mask=%u",
		    (unsigned)msg_ctrl, (unsigned)((msg_ctrl >> 15) & 0x1u),
		    (unsigned)((msg_ctrl >> 14) & 0x1u), (unsigned)new_ctrl,
		    (unsigned)((new_ctrl >> 15) & 0x1u),
		    (unsigned)((new_ctrl >> 14) & 0x1u));
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

	if (!s_msix.armed)
		return;

	pba = (volatile uint32_t *)s_msix.loc.pba;
	for (i = 0; i < s_msix.nvec; i++) {
		uk_intctlr_irq_unregister(s_msix.irqs[i], msix_tramp);
		uk_intctlr_msix_free(s_msix.irqs[i]);
	}

	{
		/* Disable MSI-X: clear bit 15 of the message control. */
		uint32_t base = s_msix.loc.msgctl_off & ~3u;
		uint32_t dw = plat_pci_cfg_read(s_msix.loc.pci_dev, base);

		plat_pci_cfg_write(s_msix.loc.pci_dev, base,
				   (dw & 0xFFFFu) | ((dw >> 16) & ~0x8000u)
							<< 16);
	}
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

/*
 * The number of vectors currently armed. Zero means the
 * platform is in software polling mode.
 */
uint32_t ena_plat_msix_state(void)
{
	return s_msix.armed ? s_msix.nvec : 0;
}

/*
 * Live readback of the MSI-X state: message control, the vector 1
 * table entry (address low, data, vector control), and the first
 * PBA dword. The heartbeat prints these so the boot-time arm log
 * does not have to survive the console capture limit.
 */
void ena_plat_msix_diag(uint32_t *msgctl, uint32_t *t1_addr, uint32_t *t1_data,
			uint32_t *t1_ctrl, uint32_t *pba0, uint32_t *irr,
			uint32_t *isr)
{
	volatile uint32_t *table;
	volatile uint32_t *pba;
	uint32_t base;
	uint32_t dw;

	*irr = 0;
	*isr = 0;
	if (!s_msix.armed) {
		*msgctl = 0;
		*t1_addr = 0;
		*t1_data = 0;
		*t1_ctrl = 0;
		*pba0 = 0;
		return;
	}

	base = s_msix.loc.msgctl_off & ~3u;
	dw = plat_pci_cfg_read(s_msix.loc.pci_dev, base);
	*msgctl = (dw >> 16) & 0xFFFFu;

	table = (volatile uint32_t *)s_msix.loc.table;
	pba = (volatile uint32_t *)s_msix.loc.pba;
	*t1_addr = table[4 * 1 + 0];
	*t1_data = table[4 * 1 + 2];
	*t1_ctrl = table[4 * 1 + 3];
	*pba0 = pba[0];

	/*
	 * LAPIC IRR/ISR bit for the vector in the table entry.
	 * Vector 0x35 is in the 32..63 range: IRR word at 0x210,
	 * ISR word at 0x110, bit (vector - 32).
	 */
	{
		volatile uint32_t *lapic = (volatile uint32_t *)0xfee00000UL;
		uint32_t vec = *t1_data & 0xFFu;

		if (vec >= 32 && vec < 64) {
			*irr = lapic[0x210 / 4] & (1u << (vec - 32));
			*isr = lapic[0x110 / 4] & (1u << (vec - 32));
		}
	}
}

#else /* !CONFIG_LIBENA_MSIX */

/*
 * Arming is off. The driver runs in software polling mode. These
 * stubs keep the platform interface linkable without the patched
 * interrupt controller.
 */
int ena_plat_msix_arm(const struct ena_msix_req *req)
{
	(void)req;
	return -ENOTSUP;
}

void ena_plat_msix_disarm(void) {}

uint32_t ena_plat_msix_vector_count(uint32_t vector)
{
	(void)vector;
	return 0;
}

uint32_t ena_plat_msix_count_get(void)
{
	return 0;
}

uint32_t ena_plat_msix_state(void)
{
	return 0;
}

void ena_plat_msix_diag(uint32_t *msgctl, uint32_t *t1_addr, uint32_t *t1_data,
			uint32_t *t1_ctrl, uint32_t *pba0, uint32_t *irr,
			uint32_t *isr)
{
	*msgctl = 0;
	*t1_addr = 0;
	*t1_data = 0;
	*t1_ctrl = 0;
	*pba0 = 0;
	*irr = 0;
	*isr = 0;
}

#endif /* CONFIG_LIBENA_MSIX */

void *ena_dma_alloc(size_t size, uint64_t *phys_out)
{
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
	while (ukplat_monotonic_clock() < deadline)
		ena_pause();
}

uint32_t ena_plat_cpu_id(void)
{
#if defined(CONFIG_LIBUKPCPUVAR) && CONFIG_LIBUKPCPUVAR
	return (uint32_t)uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
#else
	return 0;
#endif
}

#endif /* __Unikraft__ */
