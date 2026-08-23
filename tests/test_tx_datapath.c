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

static void test_tx_submit_basic(void)
{
	printf("[TEST] Running test_tx_submit_basic...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id = 999;
	const struct ena_eth_io_tx_desc *desc;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &ring) == 0);
	assert(ena_tx_free_space(ring) == 16);

	memset(&pkt, 0, sizeof(pkt));
	pkt.netbuf = (void *)0xDEADBEEF;
	pkt.phys_addr = 0x0000000180002000ULL;
	pkt.len = 128;

	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	assert(req_id == 0);
	assert(ring->sq_tail == 1);
	assert(ring->tx_packets == 1);
	assert(ring->tx_bytes == 128);
	assert(ena_tx_free_space(ring) == 15);

	/* Verify descriptor fields */
	desc = (const struct ena_eth_io_tx_desc *)ring->sq_virt;
	assert((desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_LENGTH_MASK) == 128);
	assert(desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_PHASE_MASK);
	assert(desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_FIRST_MASK);
	assert(desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_LAST_MASK);
	assert(desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_COMP_REQ_MASK);
	assert(desc[0].buff_addr_lo == 0x80002000U);
	assert(desc[0].buff_addr_hi_hdr_sz == 1U);

	/* Verify software tracking buffer */
	assert(ring->buffers.tx_bufs[0].netbuf == (void *)0xDEADBEEF);
	assert(ring->buffers.tx_bufs[0].phys_addr == 0x0000000180002000ULL);
	assert(ring->buffers.tx_bufs[0].data_len == 128);
	assert(ring->buffers.tx_bufs[0].req_id == 0);

	ena_ring_free(ring);
	printf("[PASS] test_tx_submit_basic passed\n");
}

static void test_tx_submit_checksum_flags(void)
{
	printf("[TEST] Running test_tx_submit_checksum_flags...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;
	const struct ena_eth_io_tx_desc *desc;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &ring) == 0);

	/* 1. IPv4 TCP packet with checksum offload and DF */
	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x4000;
	pkt.len = 256;
	pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
	pkt.l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
	pkt.l3_csum_en = true;
	pkt.l4_csum_en = true;
	pkt.df = true;

	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	desc = &((const struct ena_eth_io_tx_desc *)ring->sq_virt)[0];

	assert((desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK) == ENA_ETH_IO_L3_PROTO_IPV4);
	assert(((desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK) >> ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT) == ENA_ETH_IO_L4_PROTO_TCP);
	assert(desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK);
	assert(desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK);
	assert(desc->meta_ctrl & ENA_ETH_IO_TX_DESC_DF_MASK);

	/* 2. IPv6 UDP packet with checksum offload */
	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x5000;
	pkt.len = 512;
	pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
	pkt.l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
	pkt.l4_csum_en = true;

	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	desc = &((const struct ena_eth_io_tx_desc *)ring->sq_virt)[1];

	assert((desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK) == ENA_ETH_IO_L3_PROTO_IPV6);
	assert(((desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK) >> ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT) == ENA_ETH_IO_L4_PROTO_UDP);
	assert(!(desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK));
	assert(desc->meta_ctrl & ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK);

	ena_ring_free(ring);
	printf("[PASS] test_tx_submit_checksum_flags passed\n");
}

static void test_tx_queue_full(void)
{
	printf("[TEST] Running test_tx_queue_full...\n");

	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;
	int i;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x1000;
	pkt.len = 64;

	for (i = 0; i < 8; i++) {
		assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
		assert(req_id == (uint16_t)i);
	}

	assert(ena_tx_free_space(ring) == 0);
	assert(ena_tx_submit(ring, &pkt, &req_id) == -EBUSY);

	ena_ring_free(ring);
	printf("[PASS] test_tx_queue_full passed\n");
}

static void test_tx_doorbell(void)
{
	printf("[TEST] Running test_tx_doorbell...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x2000;
	pkt.len = 100;

	/* Submit 3 packets */
	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	assert(ring->sq_tail == 3);

	ena_tx_doorbell(ring);
	assert(mock_ena_hw_get_reg32(&hw, ring->sq_db_offset) == 3);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_tx_doorbell passed\n");
}

static void test_tx_poll_completions(void)
{
	printf("[TEST] Running test_tx_poll_completions...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;
	unsigned int cleaned = 0;
	int i;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x1000;
	pkt.len = 64;

	/* Submit 4 packets */
	for (i = 0; i < 4; i++)
		assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	assert(ena_tx_free_space(ring) == 4);

	/* Mock finishes 2 packets */
	mock_ena_hw_emulate_tx(&hw, ring, 2);

	assert(ena_tx_poll_completions(ring, 8, &cleaned) == 2);
	assert(cleaned == 2);
	assert(ring->cq_head == 2);
	assert(ring->sq_head == 2);
	assert(ena_tx_free_space(ring) == 6);

	/* Mock finishes remaining 2 packets */
	mock_ena_hw_emulate_tx(&hw, ring, 2);

	assert(ena_tx_poll_completions(ring, 8, &cleaned) == 2);
	assert(cleaned == 2);
	assert(ring->cq_head == 4);
	assert(ring->sq_head == 4);
	assert(ena_tx_free_space(ring) == 8);

	/* No more completions */
	assert(ena_tx_poll_completions(ring, 8, &cleaned) == 0);
	assert(cleaned == 0);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_tx_poll_completions passed\n");
}

static void test_tx_phase_flip_wrap(void)
{
	printf("[TEST] Running test_tx_phase_flip_wrap...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;
	unsigned int cleaned;
	const struct ena_eth_io_tx_desc *desc;
	int i;

	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 4, 4, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x3000;
	pkt.len = 50;

	/* Cycle 1: Submit 4 packets (phase = 1) */
	for (i = 0; i < 4; i++)
		assert(ena_tx_submit(ring, &pkt, &req_id) == 0);

	assert(ring->sq_tail == 4);
	assert((ring->sq_tail & (ring->sq_depth - 1)) == 0);
	assert(ring->sq_phase == 0); /* flipped to 0 on wrap */

	/* Complete 4 packets */
	mock_ena_hw_emulate_tx(&hw, ring, 4);
	assert(ena_tx_poll_completions(ring, 4, &cleaned) == 4);
	assert(ring->cq_head == 4);
	assert((ring->cq_head & (ring->cq_depth - 1)) == 0);
	assert(ring->cq_phase == 0); /* flipped to 0 on wrap */

	/* Cycle 2: Submit next packet with phase = 0 */
	assert(ena_tx_submit(ring, &pkt, &req_id) == 0);
	desc = &((const struct ena_eth_io_tx_desc *)ring->sq_virt)[0];
	assert(!(desc->len_ctrl & ENA_ETH_IO_TX_DESC_PHASE_MASK)); /* Phase bit is 0 */

	/* Complete the 5th packet */
	mock_ena_hw_emulate_tx(&hw, ring, 1);
	assert(ena_tx_poll_completions(ring, 4, &cleaned) == 1);
	assert(ring->cq_head == 5);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_tx_phase_flip_wrap passed\n");
}

static void test_tx_invalid_args(void)
{
	printf("[TEST] Running test_tx_invalid_args...\n");

	struct ena_adapter adapter;
	struct ena_ring *tx_ring = NULL;
	struct ena_ring *rx_ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;

	memset(&adapter, 0, sizeof(adapter));
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &tx_ring) == 0);
	assert(ena_ring_alloc(&adapter, 1, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x1000;
	pkt.len = 64;

	/* Null ring or pkt */
	assert(ena_tx_submit(NULL, &pkt, &req_id) == -EINVAL);
	assert(ena_tx_submit(tx_ring, NULL, &req_id) == -EINVAL);

	/* Invalid length */
	pkt.len = 0;
	assert(ena_tx_submit(tx_ring, &pkt, &req_id) == -EINVAL);
	pkt.len = 0x10000;
	assert(ena_tx_submit(tx_ring, &pkt, &req_id) == -EINVAL);
	pkt.len = 64;

	/* Wrong ring type */
	assert(ena_tx_submit(rx_ring, &pkt, &req_id) == -EINVAL);
	assert(ena_tx_poll_completions(rx_ring, 8, NULL) == -EINVAL);

	ena_ring_free(tx_ring);
	ena_ring_free(rx_ring);

	printf("[PASS] test_tx_invalid_args passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 5 Test Suite \n");
	printf("========================================\n");

	test_tx_submit_basic();
	test_tx_submit_checksum_flags();
	test_tx_queue_full();
	test_tx_doorbell();
	test_tx_poll_completions();
	test_tx_phase_flip_wrap();
	test_tx_invalid_args();

	printf("========================================\n");
	printf("ALL PHASE 5 TX TESTS PASSED (7/7)       \n");
	printf("========================================\n");
	return 0;
}
