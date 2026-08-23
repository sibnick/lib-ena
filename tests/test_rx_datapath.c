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

static void *mock_alloc_netbuf_helper(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0x1000000;
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x1000;
	*len_out = 2048;
	return (void *)(uintptr_t)*phys_out;
}

static void test_rx_submit_one_basic(void)
{
	printf("[TEST] Running test_rx_submit_one_basic...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	uint16_t req_id = 999;
	const struct ena_eth_io_rx_desc *desc;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 16, 16, &ring) == 0);
	assert(ena_rx_free_space(ring) == 16);

	assert(ena_rx_submit_one(ring, (void *)0xCAFEBABE, 0x0000000240008000ULL, 2048, &req_id) == 0);
	assert(req_id == 0);
	assert(ring->sq_tail == 1);
	assert(ena_rx_free_space(ring) == 15);

	/* Verify RX descriptor */
	desc = (const struct ena_eth_io_rx_desc *)ring->sq_virt;
	assert(desc[0].length == 2048);
	assert(desc[0].ctrl & ENA_ETH_IO_RX_DESC_PHASE_MASK);
	assert(desc[0].ctrl & ENA_ETH_IO_RX_DESC_FIRST_MASK);
	assert(desc[0].ctrl & ENA_ETH_IO_RX_DESC_LAST_MASK);
	assert(desc[0].ctrl & ENA_ETH_IO_RX_DESC_COMP_REQ_MASK);
	assert(desc[0].req_id == 0);
	assert(desc[0].buff_addr_lo == 0x40008000U);
	assert(desc[0].buff_addr_hi == 2U);

	/* Verify software buffer tracking */
	assert(ring->buffers.rx_bufs[0].netbuf == (void *)0xCAFEBABE);
	assert(ring->buffers.rx_bufs[0].phys_addr == 0x0000000240008000ULL);
	assert(ring->buffers.rx_bufs[0].data_len == 2048);
	assert(ring->buffers.rx_bufs[0].req_id == 0);

	ena_ring_free(ring);
	printf("[PASS] test_rx_submit_one_basic passed\n");
}

static void test_rx_refill_batch(void)
{
	printf("[TEST] Running test_rx_refill_batch...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	unsigned int refilled = 0;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &ring) == 0);

	assert(ena_rx_refill(ring, 8, mock_alloc_netbuf_helper, NULL, &refilled) == 8);
	assert(refilled == 8);
	assert(ring->sq_tail == 8);
	assert((ring->sq_tail & (ring->sq_depth - 1)) == 0);
	assert(ring->sq_phase == 0); /* flipped phase on ring wrap */
	assert(ena_rx_free_space(ring) == 0);

	/* Further refill returns 0 (ring full) */
	assert(ena_rx_refill(ring, 8, mock_alloc_netbuf_helper, NULL, &refilled) == 0);
	assert(refilled == 0);

	ena_ring_free(ring);
	printf("[PASS] test_rx_refill_batch passed\n");
}

static void test_rx_doorbell(void)
{
	printf("[TEST] Running test_rx_doorbell...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	uint16_t req_id;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 16, 16, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* Submit 5 buffers */
	assert(ena_rx_submit_one(ring, (void *)1, 0x1000, 1500, &req_id) == 0);
	assert(ena_rx_submit_one(ring, (void *)2, 0x2000, 1500, &req_id) == 0);
	assert(ena_rx_submit_one(ring, (void *)3, 0x3000, 1500, &req_id) == 0);
	assert(ena_rx_submit_one(ring, (void *)4, 0x4000, 1500, &req_id) == 0);
	assert(ena_rx_submit_one(ring, (void *)5, 0x5000, 1500, &req_id) == 0);
	assert(ring->sq_tail == 5);

	ena_rx_doorbell(ring);
	assert(mock_ena_hw_get_reg32(&hw, ring->sq_db_offset) == 5);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_rx_doorbell passed\n");
}

static void test_rx_poll_completions(void)
{
	printf("[TEST] Running test_rx_poll_completions...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_rx_pkt pkts[8];
	unsigned int refilled;
	int count;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* Populate 4 buffers */
	assert(ena_rx_refill(ring, 4, mock_alloc_netbuf_helper, NULL, &refilled) == 4);
	assert(ena_rx_free_space(ring) == 4);

	/* Mock receives 2 packets */
	mock_ena_hw_emulate_rx(&hw, ring, 2, 512, 0xABCDEF01, 0);

	count = ena_rx_poll(ring, pkts, 8);
	assert(count == 2);
	assert(pkts[0].len == 512);
	assert(pkts[0].hash == 0xABCDEF01);
	assert(pkts[0].req_id == 0);
	assert(pkts[1].len == 512);
	assert(pkts[1].hash == 0xABCDEF01);
	assert(pkts[1].req_id == 1);

	assert(ring->cq_head == 2);
	assert(ring->rx_packets == 2);
	assert(ring->rx_bytes == 1024);
	assert(ena_rx_free_space(ring) == 6); /* 2 IDs recycled */

	/* Mock receives remaining 2 packets */
	mock_ena_hw_emulate_rx(&hw, ring, 2, 1024, 0x99887766, 0);

	count = ena_rx_poll(ring, pkts, 8);
	assert(count == 2);
	assert(pkts[0].len == 1024);
	assert(pkts[0].hash == 0x99887766);
	assert(pkts[0].req_id == 2);
	assert(pkts[1].len == 1024);
	assert(pkts[1].hash == 0x99887766);
	assert(pkts[1].req_id == 3);

	assert(ring->cq_head == 4);
	assert(ring->rx_packets == 4);
	assert(ring->rx_bytes == 3072);
	assert(ena_rx_free_space(ring) == 8); /* All 4 IDs recycled */

	/* No more packets */
	assert(ena_rx_poll(ring, pkts, 8) == 0);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_rx_poll_completions passed\n");
}

static void test_rx_checksum_and_frag_flags(void)
{
	printf("[TEST] Running test_rx_checksum_and_frag_flags...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_rx_pkt pkts[4];
	unsigned int refilled;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* Populate 3 buffers */
	assert(ena_rx_refill(ring, 3, mock_alloc_netbuf_helper, NULL, &refilled) == 3);

	/* Packet 1: Checksum checked and OK */
	mock_ena_hw_emulate_rx(&hw, ring, 1, 64, 0, ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
	assert(ena_rx_poll(ring, pkts, 1) == 1);
	assert(pkts[0].l4_csum_checked == true);
	assert(pkts[0].l4_csum_err == false);
	assert(pkts[0].l3_csum_err == false);
	assert(pkts[0].frag == false);

	/* Packet 2: L3 & L4 Checksum error */
	mock_ena_hw_emulate_rx(&hw, ring, 1, 128, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK |
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK |
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
	assert(ena_rx_poll(ring, pkts, 1) == 1);
	assert(pkts[0].l3_csum_err == true);
	assert(pkts[0].l4_csum_err == true);
	assert(pkts[0].l4_csum_checked == true);
	assert(pkts[0].frag == false);

	/* Packet 3: Fragmented packet */
	mock_ena_hw_emulate_rx(&hw, ring, 1, 256, 0, ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK);
	assert(ena_rx_poll(ring, pkts, 1) == 1);
	assert(pkts[0].frag == true);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_rx_checksum_and_frag_flags passed\n");
}

static void test_rx_phase_flip_multicycle(void)
{
	printf("[TEST] Running test_rx_phase_flip_multicycle...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_rx_pkt pkts[4];
	unsigned int refilled;
	const struct ena_eth_io_rx_desc *desc;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 4, 4, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* Cycle 1: Submit 4 buffers (phase = 1) */
	assert(ena_rx_refill(ring, 4, mock_alloc_netbuf_helper, NULL, &refilled) == 4);
	assert(ring->sq_tail == 4);
	assert((ring->sq_tail & (ring->sq_depth - 1)) == 0);
	assert(ring->sq_phase == 0); /* flipped */

	/* Complete 4 packets (phase = 1) */
	mock_ena_hw_emulate_rx(&hw, ring, 4, 100, 0, 0);
	assert(ena_rx_poll(ring, pkts, 4) == 4);
	assert(ring->cq_head == 4);
	assert((ring->cq_head & (ring->cq_depth - 1)) == 0);
	assert(ring->cq_phase == 0); /* flipped */

	/* Cycle 2: Submit 4 buffers (phase = 0) */
	assert(ena_rx_refill(ring, 4, mock_alloc_netbuf_helper, NULL, &refilled) == 4);
	desc = (const struct ena_eth_io_rx_desc *)ring->sq_virt;
	assert(!(desc[0].ctrl & ENA_ETH_IO_RX_DESC_PHASE_MASK)); /* Phase 0 */
	assert(ring->sq_tail == 8);
	assert((ring->sq_tail & (ring->sq_depth - 1)) == 0);
	assert(ring->sq_phase == 1); /* flipped back to 1 */

	/* Complete 4 packets (phase = 0) */
	mock_ena_hw_emulate_rx(&hw, ring, 4, 200, 0, 0);
	assert(ena_rx_poll(ring, pkts, 4) == 4);
	assert(ring->cq_head == 8);
	assert((ring->cq_head & (ring->cq_depth - 1)) == 0);
	assert(ring->cq_phase == 1); /* flipped back to 1 */

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_rx_phase_flip_multicycle passed\n");
}

static void test_rx_invalid_args(void)
{
	printf("[TEST] Running test_rx_invalid_args...\n");

	struct ena_adapter adapter;
	struct ena_ring *tx_ring = NULL;
	struct ena_ring *rx_ring = NULL;
	struct ena_rx_pkt pkts[4];
	uint16_t req_id;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &tx_ring) == 0);
	assert(ena_ring_alloc(&adapter, 1, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);

	/* Null ring or buffer */
	assert(ena_rx_submit_one(NULL, (void *)1, 0x1000, 1500, &req_id) == -EINVAL);
	assert(ena_rx_submit_one(rx_ring, NULL, 0x1000, 1500, &req_id) == -EINVAL);

	/* Invalid length */
	assert(ena_rx_submit_one(rx_ring, (void *)1, 0x1000, 0, &req_id) == -EINVAL);
	assert(ena_rx_submit_one(rx_ring, (void *)1, 0x1000, 0x10000, &req_id) == -EINVAL);

	/* Wrong ring type */
	assert(ena_rx_submit_one(tx_ring, (void *)1, 0x1000, 1500, &req_id) == -EINVAL);
	assert(ena_rx_refill(tx_ring, 4, mock_alloc_netbuf_helper, NULL, NULL) == -EINVAL);
	assert(ena_rx_poll(tx_ring, pkts, 4) == -EINVAL);

	ena_ring_free(tx_ring);
	ena_ring_free(rx_ring);

	printf("[PASS] test_rx_invalid_args passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 6 Test Suite \n");
	printf("========================================\n");

	test_rx_submit_one_basic();
	test_rx_refill_batch();
	test_rx_doorbell();
	test_rx_poll_completions();
	test_rx_checksum_and_frag_flags();
	test_rx_phase_flip_multicycle();
	test_rx_invalid_args();

	printf("========================================\n");
	printf("ALL PHASE 6 RX TESTS PASSED (7/7)       \n");
	printf("========================================\n");
	return 0;
}
