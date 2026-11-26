/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_INTR_H
#define LIBENA_ENA_INTR_H

#include "ena.h"

#include <stdint.h>
#include <stdbool.h>

#define ENA_MAX_MSIX_VECTORS 32

/* Interrupt control register written to the per-queue unmask register
 * reported by the CREATE_CQ response. Bit 30 unmasking the interrupt
 * of the queue. Mirrors reference/ena_eth_io_defs.h,
 * struct ena_eth_io_intr_reg. */
#define ENA_ETH_IO_INTR_REG_INTR_UNMASK_SHIFT 30
#define ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK                                   \
	(1u << ENA_ETH_IO_INTR_REG_INTR_UNMASK_SHIFT)

/* MSI-X Interrupt Vector descriptor */
struct ena_irq_vector {
	uint32_t vector_id;
	uint16_t queue_id;
	bool is_admin;
	bool masked;
	uint32_t moderation_interval_usec;
	uint32_t intr_count;
};

typedef void (*ena_rx_handler_t)(void *arg, uint16_t qid,
				 struct ena_rx_pkt *pkt);

/* Polling engine execution context */
struct ena_poll_ctx {
	struct ena_adapter *adapter;
	unsigned int tx_budget;
	unsigned int rx_budget;
	uint64_t total_tx_cleaned;
	uint64_t total_rx_received;
	ena_rx_handler_t rx_handler;
	void *rx_arg;
};

/**
 * Allocate and initialize the MSI-X interrupt vector table on the adapter.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param num_vectors Number of MSI-X interrupt vectors to allocate.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_intr_msix_init(struct ena_adapter *adapter, uint32_t num_vectors);

/**
 * Free the MSI-X interrupt vector table on the adapter.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 */
void ena_intr_msix_fini(struct ena_adapter *adapter);

/**
 * Set up MSI-X interrupts for the adapter.
 *
 * The setup probes the platform for usable MSI-X vectors. When vectors are
 * available, the driver allocates the vector table and enables the device
 * interrupt for the admin vector. When the platform provides no vectors,
 * the driver stays in software polling mode.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param pci_dev Platform-specific PCI device handle.
 * @return 0 when MSI-X is active, or a negative errno value when the
 *         platform provides no MSI-X support.
 */
int ena_intr_setup(struct ena_adapter *adapter, void *pci_dev);

/**
 * Compute the lcpu target list for the MSI-X vectors of one device.
 *
 * Vector 0 carries the Admin/AENQ events. It always targets lcpu 0.
 * Vector i+1 carries the completion events of IO queue pair i. When
 * lcpu_list is empty, the function reproduces the modulo rule: queue
 * q targets lcpu q % ncpus. When lcpu_list holds a comma-separated
 * list of lcpu numbers, queue q takes the entry at position q, and
 * the function cycles a list that is shorter than the queue count.
 * Spaces around list entries are valid. A trailing comma, a
 * non-digit entry, or an entry that reaches ncpus is invalid. The
 * decoupled list lets a kernel boot 2N logical CPUs while Q queue
 * pairs keep interrupts on N distinct cores. The booted-CPU count
 * alone cannot express that. [Ticket 344310cd80]
 *
 * @param nvec      Number of MSI-X vectors. It must fit
 *                  ENA_MAX_MSIX_VECTORS.
 * @param lcpu_list Comma-separated lcpu target list. Empty string
 *                  selects the modulo rule.
 * @param ncpus     Logical CPU count the targets must fit into. A
 *                  value below one acts as one.
 * @param out_lcpu  Output array with space for nvec entries.
 * @return Number of list entries used, or 0 for the modulo rule.
 *         Returns -EINVAL for invalid arguments or a malformed list.
 */
int ena_intr_msix_targets(uint32_t nvec, const char *lcpu_list, uint32_t ncpus,
			  uint32_t *out_lcpu);

/**
 * Mask an individual MSI-X interrupt vector.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param vector_id Index of the MSI-X vector to mask.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_intr_mask_vector(struct ena_adapter *adapter, uint32_t vector_id);

/**
 * Unmask an individual MSI-X interrupt vector.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param vector_id Index of the MSI-X vector to unmask.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_intr_unmask_vector(struct ena_adapter *adapter, uint32_t vector_id);

/**
 * Mask all configured MSI-X interrupt vectors on the adapter.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 */
void ena_intr_mask_all(struct ena_adapter *adapter);

/**
 * Unmask all configured MSI-X interrupt vectors on the adapter.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 */
void ena_intr_unmask_all(struct ena_adapter *adapter);

/**
 * Set the interrupt moderation coalescing timer for an MSI-X vector.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param vector_id Index of the MSI-X vector to configure.
 * @param usecs Moderation interval in microseconds.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_intr_set_coalesce(struct ena_adapter *adapter, uint32_t vector_id,
			  uint32_t usecs);

/**
 * Execute one non-blocking polling sweep across all active TX and RX queues.
 *
 * @param ctx Pointer to the polling engine execution context.
 * @param work_done Output pointer storing the total number of processed
 * packets.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_poll_step(struct ena_poll_ctx *ctx, unsigned int *work_done);

#endif /* LIBENA_ENA_INTR_H */
