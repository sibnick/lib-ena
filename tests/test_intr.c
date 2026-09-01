/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_intr.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *mock_rx_alloc_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0x9000000;
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x1000;
	*len_out = 2048;
	return (void *)(uintptr_t)*phys_out;
}

static int setup_test_adapter(struct mock_ena_hw *hw, struct ena_adapter *adapter)
{
	mock_ena_hw_init(hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, hw);

	int ret = ena_device_init_scaffold(adapter, hw->bar0, sizeof(hw->bar0));
	if (ret)
		return ret;

	ret = ena_admin_init(adapter, 8, 8, 8);
	if (ret)
		return ret;

	ret = ena_init_run(adapter, 1500);
	if (ret)
		return ret;

	adapter->rx_rings = calloc(adapter->max_rx_queues, sizeof(struct ena_ring *));
	adapter->tx_rings = calloc(adapter->max_tx_queues, sizeof(struct ena_ring *));
	return 0;
}

static void teardown_test_adapter(struct ena_adapter *adapter)
{
	if (adapter->rx_rings) {
		for (uint16_t i = 0; i < adapter->max_rx_queues; i++) {
			if (adapter->rx_rings[i])
				ena_ring_free(adapter->rx_rings[i]);
		}
		free(adapter->rx_rings);
		adapter->rx_rings = NULL;
	}

	if (adapter->tx_rings) {
		for (uint16_t i = 0; i < adapter->max_tx_queues; i++) {
			if (adapter->tx_rings[i])
				ena_ring_free(adapter->tx_rings[i]);
		}
		free(adapter->tx_rings);
		adapter->tx_rings = NULL;
	}

	ena_admin_fini(adapter);
}

static void test_intr_msix_init_and_fini(void)
{
	printf("[TEST] Running test_intr_msix_init_and_fini...\n");

	struct ena_adapter adapter;
	memset(&adapter, 0, sizeof(adapter));

	assert(ena_intr_msix_init(&adapter, 4) == 0);
	assert(adapter.num_irq_vectors == 4);
	assert(adapter.irq_vectors != NULL);

	/* Vector 0 is admin */
	assert(adapter.irq_vectors[0].is_admin == true);
	assert(adapter.irq_vectors[0].vector_id == 0);
	assert(adapter.irq_vectors[0].masked == true);

	/* Vector 1..3 are IO */
	assert(adapter.irq_vectors[1].is_admin == false);
	assert(adapter.irq_vectors[1].queue_id == 0);
	assert(adapter.irq_vectors[1].moderation_interval_usec == 20);

	assert(adapter.irq_vectors[2].queue_id == 1);
	assert(adapter.irq_vectors[3].queue_id == 2);

	ena_intr_msix_fini(&adapter);
	assert(adapter.irq_vectors == NULL);
	assert(adapter.num_irq_vectors == 0);

	printf("[PASS] test_intr_msix_init_and_fini passed\n");
}

/* Masking and unmasking the admin vector write the INTR_MASK register
 * (BAR0 0x4C) with the ENA spec polarity: 1 = masked, 0 = unmasked. */
static void test_intr_intr_mask_register(void)
{
	printf("[TEST] Running test_intr_intr_mask_register...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	assert(setup_test_adapter(&hw, &adapter) == 0);
	assert(ena_intr_msix_init(&adapter, 4) == 0);

	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 0);

	/* Mask the admin vector: INTR_MASK bit 0 set. */
	assert(ena_intr_mask_vector(&adapter, 0) == 0);
	assert(adapter.irq_vectors[0].masked == true);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 1);

	/* Unmask the admin vector: INTR_MASK bit 0 clear. */
	assert(ena_intr_unmask_vector(&adapter, 0) == 0);
	assert(adapter.irq_vectors[0].masked == false);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 0);

	/* Fini masks every vector, including the admin vector. */
	ena_intr_msix_fini(&adapter);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 1);

	teardown_test_adapter(&adapter);
	printf("[PASS] test_intr_intr_mask_register passed\n");
}

/* Unmasking an IO vector writes intr_control bit 30 to the per-queue
 * unmask register reported by the CREATE_CQ response. It never writes
 * the CQ head doorbell. */
static void test_intr_unmask_cq_register(void)
{
	printf("[TEST] Running test_intr_unmask_cq_register...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	uint32_t head_db;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &adapter.tx_rings[0]) == 0);
	assert(ena_ring_create_hw(adapter.tx_rings[0], 0) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &adapter.rx_rings[0]) == 0);
	assert(ena_ring_create_hw(adapter.rx_rings[0], 0) == 0);

	/* The device reported a per-queue unmask register for both CQs,
	 * distinct from each CQ head doorbell. */
	assert(adapter.tx_rings[0]->cq_unmask_db_offset != 0);
	assert(adapter.rx_rings[0]->cq_unmask_db_offset != 0);
	assert(adapter.tx_rings[0]->cq_unmask_db_offset !=
	       adapter.tx_rings[0]->cq_db_offset);
	assert(adapter.rx_rings[0]->cq_unmask_db_offset !=
	       adapter.rx_rings[0]->cq_db_offset);

	assert(ena_intr_msix_init(&adapter, 2) == 0);

	/* Seed the CQ head doorbell so any write to it is visible. The
	 * mock gives both CQs the same head doorbell offset, so one seed
	 * covers both rings. */
	ena_reg_write32(adapter.tx_rings[0]->cq_db, 3);
	head_db = mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->cq_db_offset);
	assert(head_db == 3);
	assert(mock_ena_hw_get_reg32(&hw, adapter.rx_rings[0]->cq_db_offset) == 3);

	/* Unmask vector 1 (queue 0). */
	assert(ena_intr_unmask_vector(&adapter, 1) == 0);
	assert(adapter.irq_vectors[1].masked == false);

	/* intr_control bit 30 written to both per-queue unmask registers. */
	assert(mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->cq_unmask_db_offset) ==
	       ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
	assert(mock_ena_hw_get_reg32(&hw, adapter.rx_rings[0]->cq_unmask_db_offset) ==
	       ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);

	/* The CQ head doorbell is unchanged. */
	assert(mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->cq_db_offset) == head_db);
	assert(mock_ena_hw_get_reg32(&hw, adapter.rx_rings[0]->cq_db_offset) == head_db);

	ena_intr_msix_fini(&adapter);
	ena_ring_destroy_hw(adapter.tx_rings[0]);
	ena_ring_destroy_hw(adapter.rx_rings[0]);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_intr_unmask_cq_register passed\n");
}

static void test_intr_set_coalesce(void)
{
	printf("[TEST] Running test_intr_set_coalesce...\n");

	struct ena_adapter adapter;
	memset(&adapter, 0, sizeof(adapter));

	assert(ena_intr_msix_init(&adapter, 4) == 0);

	assert(ena_intr_set_coalesce(&adapter, 1, 50) == 0);
	assert(adapter.irq_vectors[1].moderation_interval_usec == 50);

	/* Invalid vector ID */
	assert(ena_intr_set_coalesce(&adapter, 10, 50) == -EINVAL);

	ena_intr_msix_fini(&adapter);
	printf("[PASS] test_intr_set_coalesce passed\n");
}

static void test_poll_step_engine(void)
{
	printf("[TEST] Running test_poll_step_engine...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_poll_ctx ctx;
	struct ena_tx_pkt tx_pkt;
	unsigned int work_done = 0;
	unsigned int refilled = 0;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &adapter.tx_rings[0]) == 0);
	assert(ena_ring_create_hw(adapter.tx_rings[0], 0) == 0);

	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &adapter.rx_rings[0]) == 0);
	assert(ena_ring_create_hw(adapter.rx_rings[0], 0) == 0);

	/* Populate 2 RX buffers */
	assert(ena_rx_refill(adapter.rx_rings[0], 2, mock_rx_alloc_cb, NULL, &refilled) == 2);

	/* Submit 2 TX packets */
	memset(&tx_pkt, 0, sizeof(tx_pkt));
	tx_pkt.phys_addr = 0x1000;
	tx_pkt.len = 128;
	assert(ena_tx_submit(adapter.tx_rings[0], &tx_pkt, NULL) == 0);
	assert(ena_tx_submit(adapter.tx_rings[0], &tx_pkt, NULL) == 0);

	/* Mock device completes 2 TX and produces 2 RX */
	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 2);
	mock_ena_hw_emulate_rx(&hw, adapter.rx_rings[0], 2, 64, 0, 0);

	/* Execute polling step */
	memset(&ctx, 0, sizeof(ctx));
	ctx.adapter = &adapter;
	ctx.tx_budget = 16;
	ctx.rx_budget = 16;

	assert(ena_poll_step(&ctx, &work_done) == 4);
	assert(work_done == 4);
	assert(ctx.total_tx_cleaned == 2);
	assert(ctx.total_rx_received == 2);

	/* Next polling step has 0 work */
	assert(ena_poll_step(&ctx, &work_done) == 0);
	assert(work_done == 0);

	assert(ena_ring_destroy_hw(adapter.tx_rings[0]) == 0);
	assert(ena_ring_destroy_hw(adapter.rx_rings[0]) == 0);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_poll_step_engine passed\n");
}

static void test_intr_invalid_args(void)
{
	printf("[TEST] Running test_intr_invalid_args...\n");

	struct ena_adapter adapter;
	memset(&adapter, 0, sizeof(adapter));

	assert(ena_intr_msix_init(NULL, 4) == -EINVAL);
	assert(ena_intr_msix_init(&adapter, 0) == -EINVAL);
	assert(ena_intr_msix_init(&adapter, 33) == -EINVAL);

	assert(ena_intr_mask_vector(NULL, 0) == -EINVAL);
	assert(ena_intr_unmask_vector(NULL, 0) == -EINVAL);
	assert(ena_poll_step(NULL, NULL) == -EINVAL);

	printf("[PASS] test_intr_invalid_args passed\n");
}

static void test_intr_setup_msix(void)
{
	printf("[TEST] Running test_intr_setup_msix...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	/* Platform provides no vectors: stay in software polling mode. */
	ena_plat_set_mock_msix_vectors(0);
	assert(ena_intr_setup(&adapter, NULL) == -ENOTSUP);
	assert(adapter.irq_vectors == NULL);
	assert(adapter.num_irq_vectors == 0);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 0);

	/* Platform provides 4 vectors: allocate the table and unmask
	 * the admin vector (INTR_MASK bit 0 clear). IO vectors stay
	 * masked. */
	ena_plat_set_mock_msix_vectors(4);
	assert(ena_intr_setup(&adapter, NULL) == 0);
	assert(adapter.num_irq_vectors == 4);
	assert(adapter.irq_vectors[0].is_admin == true);
	assert(adapter.irq_vectors[0].masked == false);
	assert(adapter.irq_vectors[1].masked == true);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 0);

	/* A second setup call is a no-op. */
	assert(ena_intr_setup(&adapter, NULL) == 0);
	assert(adapter.num_irq_vectors == 4);

	/* Fini re-masks the admin vector (INTR_MASK bit 0 set). */
	ena_intr_msix_fini(&adapter);
	assert(adapter.irq_vectors == NULL);
	assert(adapter.num_irq_vectors == 0);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_INTR_MASK_OFF) == 1);

	/* A vector count above the maximum is clamped. */
	ena_plat_set_mock_msix_vectors(128);
	assert(ena_intr_setup(&adapter, NULL) == 0);
	assert(adapter.num_irq_vectors == ENA_MAX_MSIX_VECTORS);

	ena_intr_msix_fini(&adapter);
	ena_plat_set_mock_msix_vectors(0);

	teardown_test_adapter(&adapter);
	printf("[PASS] test_intr_setup_msix passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 8 Test Suite \n");
	printf("========================================\n");

	test_intr_msix_init_and_fini();
	test_intr_intr_mask_register();
	test_intr_unmask_cq_register();
	test_intr_set_coalesce();
	test_poll_step_engine();
	test_intr_invalid_args();
	test_intr_setup_msix();

	printf("========================================\n");
	printf("ALL PHASE 8 INTR TESTS PASSED (7/7)     \n");
	printf("========================================\n");
	return 0;
}
