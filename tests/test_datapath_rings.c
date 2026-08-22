/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_datapath.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int setup_adapter(struct mock_ena_hw *hw, struct ena_adapter *adapter)
{
	mock_ena_hw_init(hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, hw);

	int ret = ena_device_init_scaffold(adapter, hw->bar0, sizeof(hw->bar0));
	if (ret)
		return ret;

	return ena_admin_init(adapter, 8, 8, 8);
}

static void test_datapath_wire_layouts(void)
{
	printf("[TEST] Running test_datapath_wire_layouts...\n");

	/* IO Descriptors */
	assert(sizeof(struct ena_eth_io_tx_desc) == 16);
	assert(sizeof(struct ena_eth_io_tx_cdesc) == 8);
	assert(sizeof(struct ena_eth_io_rx_desc) == 16);
	assert(sizeof(struct ena_eth_io_rx_cdesc_base) == 16);
	assert(sizeof(struct ena_eth_io_rx_cdesc_ext) == 32);

	/* Admin CQ commands */
	assert(sizeof(struct ena_admin_aq_create_cq_cmd) == 20);
	assert(sizeof(struct ena_admin_acq_create_cq_resp_desc) == 24);
	assert(sizeof(struct ena_admin_aq_destroy_cq_cmd) == 8);
	assert(sizeof(struct ena_admin_acq_destroy_cq_resp_desc) == 8);

	/* Admin SQ commands */
	assert(sizeof(struct ena_admin_aq_create_sq_cmd) == 36);
	assert(sizeof(struct ena_admin_acq_create_sq_resp_desc) == 24);
	assert(sizeof(struct ena_admin_aq_destroy_sq_cmd) == 8);
	assert(sizeof(struct ena_admin_acq_destroy_sq_resp_desc) == 8);

	printf("[PASS] test_datapath_wire_layouts passed\n");
}

static void test_ring_alloc_free_tx(void)
{
	printf("[TEST] Running test_ring_alloc_free_tx...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	memset(&adapter, 0, sizeof(adapter));

	int ret = ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 32, 32, &ring);
	assert(ret == 0);
	assert(ring != NULL);
	assert(ring->adapter == &adapter);
	assert(ring->qid == 0);
	assert(ring->ring_type == ENA_RING_TYPE_TX);
	assert(ring->sq_depth == 32);
	assert(ring->cq_depth == 32);
	assert(ring->sq_virt != NULL);
	assert(ring->sq_phys != 0);
	assert(ring->cq_virt != NULL);
	assert(ring->cq_phys != 0);
	assert(ring->free_req_ids != NULL);
	assert(ring->free_req_count == 32);
	assert(ring->buffers.tx_bufs != NULL);

	ena_ring_free(ring);
	printf("[PASS] test_ring_alloc_free_tx passed\n");
}

static void test_ring_alloc_free_rx(void)
{
	printf("[TEST] Running test_ring_alloc_free_rx...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	memset(&adapter, 0, sizeof(adapter));

	int ret = ena_ring_alloc(&adapter, 1, ENA_RING_TYPE_RX, 64, 64, &ring);
	assert(ret == 0);
	assert(ring != NULL);
	assert(ring->qid == 1);
	assert(ring->ring_type == ENA_RING_TYPE_RX);
	assert(ring->sq_depth == 64);
	assert(ring->cq_depth == 64);
	assert(ring->sq_virt != NULL);
	assert(ring->cq_virt != NULL);
	assert(ring->buffers.rx_bufs != NULL);
	assert(ring->free_req_count == 64);

	ena_ring_free(ring);
	printf("[PASS] test_ring_alloc_free_rx passed\n");
}

static void test_ring_alloc_bad_depth(void)
{
	printf("[TEST] Running test_ring_alloc_bad_depth...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	memset(&adapter, 0, sizeof(adapter));

	/* Null arguments */
	assert(ena_ring_alloc(NULL, 0, ENA_RING_TYPE_TX, 32, 32, &ring) == -EINVAL);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 32, 32, NULL) == -EINVAL);

	/* Non-power-of-two depths */
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 0, 32, &ring) == -EINVAL);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 3, 32, &ring) == -EINVAL);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 32, 5, &ring) == -EINVAL);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 33, 32, &ring) == -EINVAL);

	/* Invalid ring type */
	assert(ena_ring_alloc(&adapter, 0, (enum ena_ring_type)99, 32, 32, &ring) == -EINVAL);

	printf("[PASS] test_ring_alloc_bad_depth passed\n");
}

static void test_req_id_pool(void)
{
	printf("[TEST] Running test_req_id_pool...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	uint16_t ids[8];
	uint16_t id;
	int i;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);

	/* Allocate all 8 request IDs */
	for (i = 0; i < 8; i++) {
		assert(ena_ring_req_id_alloc(ring, &ids[i]) == 0);
		assert(ids[i] == (uint16_t)i);
	}
	assert(ring->free_req_count == 0);

	/* 9th allocation should fail with -EBUSY */
	assert(ena_ring_req_id_alloc(ring, &id) == -EBUSY);

	/* Free request ID out of order */
	assert(ena_ring_req_id_free(ring, ids[3]) == 0);
	assert(ring->free_req_count == 1);
	assert(ena_ring_req_id_free(ring, ids[0]) == 0);
	assert(ring->free_req_count == 2);

	/* Reallocate freed request IDs */
	assert(ena_ring_req_id_alloc(ring, &id) == 0);
	assert(id == ids[3]);
	assert(ena_ring_req_id_alloc(ring, &id) == 0);
	assert(id == ids[0]);
	assert(ring->free_req_count == 0);

	/* Free invalid req_id */
	assert(ena_ring_req_id_free(ring, 100) == -EINVAL);

	/* Free remaining */
	for (i = 0; i < 8; i++) {
		if (i != 0 && i != 3)
			assert(ena_ring_req_id_free(ring, ids[i]) == 0);
	}
	assert(ena_ring_req_id_free(ring, ids[0]) == 0);
	assert(ena_ring_req_id_free(ring, ids[3]) == 0);
	assert(ring->free_req_count == 8);

	/* Extra free fails */
	assert(ena_ring_req_id_free(ring, 0) == -EINVAL);

	ena_ring_free(ring);
	printf("[PASS] test_req_id_pool passed\n");
}

static void test_ring_create_destroy_hw(void)
{
	printf("[TEST] Running test_ring_create_destroy_hw...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *tx_ring = NULL;
	struct ena_ring *rx_ring = NULL;

	assert(setup_adapter(&hw, &adapter) == 0);

	/* Allocate TX ring */
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &tx_ring) == 0);
	assert(ena_ring_create_hw(tx_ring, 0) == 0);

	assert(hw.cq_created_count == 1);
	assert(hw.sq_created_count == 1);
	assert(hw.last_sq_direction == ENA_ADMIN_SQ_DIRECTION_TX);
	assert(tx_ring->sq_idx == 0);
	assert(tx_ring->cq_idx == 0);
	assert(tx_ring->sq_db != NULL);
	assert(tx_ring->cq_db != NULL);

	/* Allocate RX ring */
	assert(ena_ring_alloc(&adapter, 1, ENA_RING_TYPE_RX, 32, 32, &rx_ring) == 0);
	assert(ena_ring_create_hw(rx_ring, 1) == 0);

	assert(hw.cq_created_count == 2);
	assert(hw.sq_created_count == 2);
	assert(hw.last_sq_direction == ENA_ADMIN_SQ_DIRECTION_RX);
	assert(rx_ring->sq_idx == 1);
	assert(rx_ring->cq_idx == 1);
	assert(rx_ring->sq_db != NULL);
	assert(rx_ring->cq_db != NULL);

	/* Teardown hardware rings */
	assert(ena_ring_destroy_hw(tx_ring) == 0);
	assert(hw.sq_destroyed_count == 1);
	assert(hw.cq_destroyed_count == 1);
	assert(tx_ring->sq_db == NULL);
	assert(tx_ring->cq_db == NULL);

	assert(ena_ring_destroy_hw(rx_ring) == 0);
	assert(hw.sq_destroyed_count == 2);
	assert(hw.cq_destroyed_count == 2);
	assert(rx_ring->sq_db == NULL);
	assert(rx_ring->cq_db == NULL);

	ena_ring_free(tx_ring);
	ena_ring_free(rx_ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_ring_create_destroy_hw passed\n");
}

static void test_ring_create_hw_error_handling(void)
{
	printf("[TEST] Running test_ring_create_hw_error_handling...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &ring) == 0);

	/* Inject error on admin command */
	mock_ena_hw_set_admin_status(&hw, ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE);
	int ret = ena_ring_create_hw(ring, 0);
	assert(ret != 0);

	/* Clear error */
	mock_ena_hw_set_admin_status(&hw, ENA_ADMIN_SUCCESS);

	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_ring_create_hw_error_handling passed\n");
}

static void test_multiple_rings_allocation(void)
{
	printf("[TEST] Running test_multiple_rings_allocation...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *tx_rings[4];
	struct ena_ring *rx_rings[4];
	int i;

	assert(setup_adapter(&hw, &adapter) == 0);

	for (i = 0; i < 4; i++) {
		assert(ena_ring_alloc(&adapter, (uint16_t)i, ENA_RING_TYPE_TX, 16, 16,
				      &tx_rings[i]) == 0);
		assert(ena_ring_alloc(&adapter, (uint16_t)i, ENA_RING_TYPE_RX, 16, 16,
				      &rx_rings[i]) == 0);
		assert(ena_ring_create_hw(tx_rings[i], (uint32_t)i) == 0);
		assert(ena_ring_create_hw(rx_rings[i], (uint32_t)i) == 0);
	}

	assert(hw.cq_created_count == 8);
	assert(hw.sq_created_count == 8);

	for (i = 0; i < 4; i++) {
		assert(ena_ring_destroy_hw(tx_rings[i]) == 0);
		assert(ena_ring_destroy_hw(rx_rings[i]) == 0);
		ena_ring_free(tx_rings[i]);
		ena_ring_free(rx_rings[i]);
	}

	assert(hw.cq_destroyed_count == 8);
	assert(hw.sq_destroyed_count == 8);

	ena_admin_fini(&adapter);
	printf("[PASS] test_multiple_rings_allocation passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 4 Test Suite \n");
	printf("========================================\n");

	test_datapath_wire_layouts();
	test_ring_alloc_free_tx();
	test_ring_alloc_free_rx();
	test_ring_alloc_bad_depth();
	test_req_id_pool();
	test_ring_create_destroy_hw();
	test_ring_create_hw_error_handling();
	test_multiple_rings_allocation();

	printf("========================================\n");
	printf("ALL PHASE 4 DATAPATH TESTS PASSED (8/8) \n");
	printf("========================================\n");
	return 0;
}
