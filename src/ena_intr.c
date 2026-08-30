/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_intr.h"
#include "ena_datapath.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int ena_intr_msix_init(struct ena_adapter *adapter, uint32_t num_vectors)
{
	uint32_t i;

	if (!adapter || num_vectors == 0 || num_vectors > ENA_MAX_MSIX_VECTORS)
		return -EINVAL;

	adapter->irq_vectors = calloc(num_vectors, sizeof(*adapter->irq_vectors));
	if (!adapter->irq_vectors)
		return -ENOMEM;

	adapter->num_irq_vectors = num_vectors;

	/* Vector 0 is dedicated to Admin/AENQ management events */
	adapter->irq_vectors[0].vector_id = 0;
	adapter->irq_vectors[0].is_admin = true;
	adapter->irq_vectors[0].masked = true;
	adapter->irq_vectors[0].moderation_interval_usec = 0;

	/* Vectors 1..N-1 are dedicated to IO queues */
	for (i = 1; i < num_vectors; i++) {
		adapter->irq_vectors[i].vector_id = i;
		adapter->irq_vectors[i].queue_id = (uint16_t)(i - 1);
		adapter->irq_vectors[i].is_admin = false;
		adapter->irq_vectors[i].masked = true;
		adapter->irq_vectors[i].moderation_interval_usec = 20;
	}

	return 0;
}

void ena_intr_msix_fini(struct ena_adapter *adapter)
{
	if (!adapter || !adapter->irq_vectors)
		return;

	ena_intr_mask_all(adapter);
	free(adapter->irq_vectors);
	adapter->irq_vectors = NULL;
	adapter->num_irq_vectors = 0;
}

int ena_intr_mask_vector(struct ena_adapter *adapter, uint32_t vector_id)
{
	if (!adapter || !adapter->irq_vectors || vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].masked = true;

	if (adapter->bar0_base) {
		if (vector_id == 0) {
			ena_reg_write32(adapter->bar0_base + ENA_REGS_INTR_MASK_OFF, 0);
		}
	}

	return 0;
}

int ena_intr_unmask_vector(struct ena_adapter *adapter, uint32_t vector_id)
{
	uint16_t num_rx;
	uint16_t qid;

	if (!adapter || !adapter->irq_vectors || vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].masked = false;

	if (adapter->bar0_base) {
		if (vector_id == 0) {
			ena_reg_write32(adapter->bar0_base + ENA_REGS_INTR_MASK_OFF, 1);
		} else {
			qid = adapter->irq_vectors[vector_id].queue_id;
			num_rx = adapter->num_rx_rings ? adapter->num_rx_rings : adapter->max_rx_queues;
			if (adapter->rx_rings && qid < num_rx &&
			    adapter->rx_rings[qid] && adapter->rx_rings[qid]->cq_db) {
				ena_reg_write32(adapter->rx_rings[qid]->cq_db,
						adapter->rx_rings[qid]->cq_head);
			}
		}
	}

	return 0;
}

void ena_intr_mask_all(struct ena_adapter *adapter)
{
	uint32_t i;

	if (!adapter || !adapter->irq_vectors)
		return;

	for (i = 0; i < adapter->num_irq_vectors; i++)
		ena_intr_mask_vector(adapter, i);
}

void ena_intr_unmask_all(struct ena_adapter *adapter)
{
	uint32_t i;

	if (!adapter || !adapter->irq_vectors)
		return;

	for (i = 0; i < adapter->num_irq_vectors; i++)
		ena_intr_unmask_vector(adapter, i);
}

int ena_intr_set_coalesce(struct ena_adapter *adapter, uint32_t vector_id,
			  uint32_t usecs)
{
	if (!adapter || !adapter->irq_vectors || vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].moderation_interval_usec = usecs;
	return 0;
}

int ena_poll_step(struct ena_poll_ctx *ctx, unsigned int *work_done)
{
	struct ena_rx_pkt rx_pkts[32];
	unsigned int total = 0;
	unsigned int cleaned = 0;
	uint16_t num_tx;
	uint16_t num_rx;
	int count;
	int i;
	uint16_t q;

	if (!ctx || !ctx->adapter)
		return -EINVAL;

	if (ctx->tx_budget == 0)
		ctx->tx_budget = 32;
	if (ctx->rx_budget == 0)
		ctx->rx_budget = 32;

	num_tx = ctx->adapter->num_tx_rings ? ctx->adapter->num_tx_rings : ctx->adapter->max_tx_queues;
	num_rx = ctx->adapter->num_rx_rings ? ctx->adapter->num_rx_rings : ctx->adapter->max_rx_queues;

	/* TX completion polling */
	if (ctx->adapter->tx_rings) {
		for (q = 0; q < num_tx; q++) {
			if (!ctx->adapter->tx_rings[q])
				continue;

			cleaned = 0;
			ena_tx_poll_completions(ctx->adapter->tx_rings[q],
						ctx->tx_budget, &cleaned);
			total += cleaned;
			ctx->total_tx_cleaned += cleaned;
		}
	}

	/* RX packet polling */
	if (ctx->adapter->rx_rings) {
		for (q = 0; q < num_rx; q++) {
			if (!ctx->adapter->rx_rings[q])
				continue;

			count = ena_rx_poll(ctx->adapter->rx_rings[q], rx_pkts,
					    ctx->rx_budget > 32 ? 32 : ctx->rx_budget);
			if (count > 0) {
				for (i = 0; i < count; i++) {
					if (ctx->rx_handler)
						ctx->rx_handler(ctx->rx_arg, q, &rx_pkts[i]);
				}
				total += (unsigned int)count;
				ctx->total_rx_received += (uint64_t)count;
			}
		}
	}

	if (work_done)
		*work_done = total;

	return (int)total;
}
