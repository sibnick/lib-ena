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

/* MSI-X Interrupt Vector descriptor */
struct ena_irq_vector {
	uint32_t vector_id;
	uint16_t queue_id;
	bool is_admin;
	bool masked;
	uint32_t moderation_interval_usec;
	uint32_t intr_count;
};

typedef void (*ena_rx_handler_t)(void *arg, uint16_t qid, struct ena_rx_pkt *pkt);

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

/* Initialize MSI-X interrupt vector table */
int ena_intr_msix_init(struct ena_adapter *adapter, uint32_t num_vectors);

/* Free MSI-X interrupt vector table */
void ena_intr_msix_fini(struct ena_adapter *adapter);

/* Mask or unmask an individual MSI-X vector */
int ena_intr_mask_vector(struct ena_adapter *adapter, uint32_t vector_id);
int ena_intr_unmask_vector(struct ena_adapter *adapter, uint32_t vector_id);

/* Mask or unmask all configured MSI-X vectors */
void ena_intr_mask_all(struct ena_adapter *adapter);
void ena_intr_unmask_all(struct ena_adapter *adapter);

/* Set interrupt moderation coalescing interval in microseconds */
int ena_intr_set_coalesce(struct ena_adapter *adapter, uint32_t vector_id,
			  uint32_t usecs);

/* Execute one non-blocking polling sweep across all active TX and RX queues */
int ena_poll_step(struct ena_poll_ctx *ctx, unsigned int *work_done);

#endif /* LIBENA_ENA_INTR_H */
