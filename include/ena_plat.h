/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_PLAT_H
#define LIBENA_ENA_PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __Unikraft__
#include <uk/print.h>
#include <uk/alloc.h>
#include <uk/bus/pci.h>
#include <uk/netdev.h>

#define ena_info(fmt, ...)   uk_pr_info("ena: " fmt, ##__VA_ARGS__)
#define ena_warn(fmt, ...)   uk_pr_warn("ena: " fmt, ##__VA_ARGS__)
#define ena_err(fmt, ...)    uk_pr_err("ena: " fmt, ##__VA_ARGS__)
#define ena_debug(fmt, ...)  uk_pr_debug("ena: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Logging (implemented in src/ena_plat.c). These are functions, not macros,
 * so a call with no dynamic arguments is valid C99 under -pedantic. */
void ena_info(const char *fmt, ...);
void ena_warn(const char *fmt, ...);
void ena_err(const char *fmt, ...);
void ena_debug(const char *fmt, ...);
#endif

/* Memory barrier macros */
#if defined(__x86_64__) || defined(_M_X64)
#define ena_mb()    __asm__ __volatile__("mfence" ::: "memory")
#define ena_rmb()   __asm__ __volatile__("lfence" ::: "memory")
#define ena_wmb()   __asm__ __volatile__("sfence" ::: "memory")
#elif defined(__aarch64__)
#define ena_mb()    __asm__ __volatile__("dmb sy" ::: "memory")
#define ena_rmb()   __asm__ __volatile__("dmb ld" ::: "memory")
#define ena_wmb()   __asm__ __volatile__("dmb st" ::: "memory")
#else
#define ena_mb()    __asm__ __volatile__("" ::: "memory")
#define ena_rmb()   __asm__ __volatile__("" ::: "memory")
#define ena_wmb()   __asm__ __volatile__("" ::: "memory")
#endif

#define READ_ONCE32(var) \
	({ _Static_assert(sizeof(var) == 4, "READ_ONCE32 requires a 32-bit variable"); \
	   (*(const volatile uint32_t *)&(var)); })

#define WRITE_ONCE32(var, val) \
	({ _Static_assert(sizeof(var) == 4, "WRITE_ONCE32 requires a 32-bit variable"); \
	   (*(volatile uint32_t *)&(var) = (val)); })

/* CPU pause helper for spinlock loops */
static inline void ena_pause(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ena_le16_to_cpu(x) ((uint16_t)(x))
#define ena_cpu_to_le16(x) ((uint16_t)(x))
#define ena_le32_to_cpu(x) ((uint32_t)(x))
#define ena_cpu_to_le32(x) ((uint32_t)(x))
#else
#define ena_le16_to_cpu(x) __builtin_bswap16(x)
#define ena_cpu_to_le16(x) __builtin_bswap16(x)
#define ena_le32_to_cpu(x) __builtin_bswap32(x)
#define ena_cpu_to_le32(x) __builtin_bswap32(x)
#endif

/* MMIO Register accessors */
static inline uint32_t ena_reg_read32(const volatile void *addr)
{
	uint32_t val = *(const volatile uint32_t *)addr;
	ena_rmb();
	return val;
}

static inline void ena_reg_write32(volatile void *addr, uint32_t val)
{
	ena_wmb();
	*(volatile uint32_t *)addr = val;
	ena_mb();
}

/* Platform services (implemented in src/ena_plat.c) */

/**
 * Allocate a physically contiguous DMA buffer.
 *
 * @param size Allocation size in bytes.
 * @param phys_out Output pointer where the physical address is stored.
 * @return Virtual pointer to the allocated buffer, or NULL on failure.
 */
void *ena_dma_alloc(size_t size, uint64_t *phys_out);

/**
 * Free a contiguous DMA buffer previously allocated with ena_dma_alloc.
 *
 * @param virt Virtual address of the buffer to free.
 * @param phys Physical address of the buffer to free.
 */
void ena_dma_free(void *virt, uint64_t phys);

/**
 * Delay execution for a specified duration in microseconds.
 *
 * @param us Duration to delay in microseconds.
 */
void ena_delay_us(unsigned int us);

/**
 * Probe a PCI device for usable MSI-X vectors.
 *
 * The probe checks the device MSI-X capability and the platform interrupt
 * delivery support. A count of zero means the driver must stay in software
 * polling mode.
 *
 * @param pci_dev Platform-specific PCI device handle.
 * @param num_vectors Output pointer storing the number of usable vectors.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_plat_msix_probe(void *pci_dev, uint32_t *num_vectors);

#ifndef __Unikraft__
/* Host test hook: set the vector count reported by ena_plat_msix_probe. */
void ena_plat_set_mock_msix_vectors(uint32_t num_vectors);
#endif

#endif /* LIBENA_ENA_PLAT_H */

