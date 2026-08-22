/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_llq.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int setup_test_adapter_llq(struct mock_ena_hw *hw, struct ena_adapter *adapter,
				  void *bar2_buf, size_t bar2_sz)
{
	mock_ena_hw_init(hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, hw);

	int ret = ena_device_init_scaffold(adapter, hw->bar0, sizeof(hw->bar0));
	if (ret)
		return ret;

	adapter->bar2_base = bar2_buf;
	adapter->bar2_size = bar2_sz;

	ret = ena_admin_init(adapter, 8, 8, 8);
	if (ret)
		return ret;

	return ena_init_run(adapter, 1500);
}

static void test_llq_negotiation_no_bar2(void)
{
	printf("[TEST] Running test_llq_negotiation_no_bar2...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	assert(setup_test_adapter_llq(&hw, &adapter, NULL, 0) == 0);

	assert(ena_llq_negotiate(&adapter) == 0);
	assert(adapter.llq_info.supported == false);
	assert(adapter.llq_info.enabled == false);

	ena_admin_fini(&adapter);
	printf("[PASS] test_llq_negotiation_no_bar2 passed\n");
}

static void test_llq_negotiation_with_bar2(void)
{
	printf("[TEST] Running test_llq_negotiation_with_bar2...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	uint8_t bar2_memory[4096];

	assert(setup_test_adapter_llq(&hw, &adapter, bar2_memory, sizeof(bar2_memory)) == 0);

	assert(ena_llq_negotiate(&adapter) == 0);
	assert(adapter.llq_info.supported == true);
	assert(adapter.llq_info.enabled == true);
	assert(adapter.llq_info.entry_size == 128);
	assert(adapter.llq_info.header_len == 96);
	assert(adapter.llq_info.max_llq_num == 16);

	ena_admin_fini(&adapter);
	printf("[PASS] test_llq_negotiation_with_bar2 passed\n");
}

static void test_llq_tx_push_direct(void)
{
	printf("[TEST] Running test_llq_tx_push_direct...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint8_t bar2_memory[4096];
	uint8_t dummy_header[40];
	uint16_t req_id;
	const struct ena_eth_io_tx_desc *desc;
	const uint8_t *pushed_hdr;

	memset(bar2_memory, 0, sizeof(bar2_memory));
	memset(dummy_header, 0xAB, sizeof(dummy_header));

	assert(setup_test_adapter_llq(&hw, &adapter, bar2_memory, sizeof(bar2_memory)) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* Configure ring for LLQ push mode */
	ring->is_llq = true;
	ring->push_buf_virt = bar2_memory;

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x2000;
	pkt.len = 128;
	pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
	pkt.l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
	pkt.l3_csum_en = true;
	pkt.l4_csum_en = true;

	assert(ena_llq_tx_push(ring, &pkt, dummy_header, sizeof(dummy_header), &req_id) == 0);
	assert(req_id == 0);
	assert(ring->sq_tail == 1);
	assert(ring->tx_packets == 1);
	assert(mock_ena_hw_get_reg32(&hw, ring->sq_db_offset) == 1);

	/* Verify descriptor in BAR2 memory */
	desc = (const struct ena_eth_io_tx_desc *)bar2_memory;
	assert((desc->len_ctrl & ENA_ETH_IO_TX_DESC_LENGTH_MASK) == 128);
	assert(desc->len_ctrl & ENA_ETH_IO_TX_DESC_PHASE_MASK);
	assert((desc->buff_addr_hi_hdr_sz >> 24) == 40); /* Header length encoded */

	/* Verify header bytes in BAR2 memory */
	pushed_hdr = bar2_memory + sizeof(*desc);
	assert(pushed_hdr[0] == 0xAB && pushed_hdr[39] == 0xAB);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_llq_tx_push_direct passed\n");
}

static void test_llq_tx_push_fallback_standard(void)
{
	printf("[TEST] Running test_llq_tx_push_fallback_standard...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint16_t req_id;
	const struct ena_eth_io_tx_desc *desc;

	assert(setup_test_adapter_llq(&hw, &adapter, NULL, 0) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);
	assert(ena_ring_create_hw(ring, 0) == 0);

	/* LLQ is false -> should fall back to standard ring->sq_virt descriptor */
	ring->is_llq = false;

	memset(&pkt, 0, sizeof(pkt));
	pkt.phys_addr = 0x3000;
	pkt.len = 64;

	assert(ena_llq_tx_push(ring, &pkt, NULL, 0, &req_id) == 0);
	assert(req_id == 0);
	assert(ring->sq_tail == 1);

	desc = (const struct ena_eth_io_tx_desc *)ring->sq_virt;
	assert((desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_LENGTH_MASK) == 64);
	assert(mock_ena_hw_get_reg32(&hw, ring->sq_db_offset) == 1);

	assert(ena_ring_destroy_hw(ring) == 0);
	ena_ring_free(ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_llq_tx_push_fallback_standard passed\n");
}

static void test_llq_invalid_args(void)
{
	printf("[TEST] Running test_llq_invalid_args...\n");

	struct ena_adapter adapter;
	struct ena_ring tx_ring;
	struct ena_ring rx_ring;
	struct ena_tx_pkt pkt;
	uint8_t dummy_header[128];
	uint16_t req_id;

	memset(&adapter, 0, sizeof(adapter));
	memset(&tx_ring, 0, sizeof(tx_ring));
	memset(&rx_ring, 0, sizeof(rx_ring));
	memset(&pkt, 0, sizeof(pkt));

	tx_ring.ring_type = ENA_RING_TYPE_TX;
	tx_ring.is_llq = true;
	tx_ring.push_buf_virt = dummy_header;
	rx_ring.ring_type = ENA_RING_TYPE_RX;

	assert(ena_llq_negotiate(NULL) == -EINVAL);

	/* NULL ring or pkt */
	assert(ena_llq_tx_push(NULL, &pkt, NULL, 0, &req_id) == -EINVAL);
	assert(ena_llq_tx_push(&tx_ring, NULL, NULL, 0, &req_id) == -EINVAL);

	/* Wrong ring type */
	assert(ena_llq_tx_push(&rx_ring, &pkt, NULL, 0, &req_id) == -EINVAL);

	/* Header length > 96 */
	assert(ena_llq_tx_push(&tx_ring, &pkt, dummy_header, 100, &req_id) == -EINVAL);

	printf("[PASS] test_llq_invalid_args passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 9 Test Suite \n");
	printf("========================================\n");

	test_llq_negotiation_no_bar2();
	test_llq_negotiation_with_bar2();
	test_llq_tx_push_direct();
	test_llq_tx_push_fallback_standard();
	test_llq_invalid_args();

	printf("========================================\n");
	printf("ALL PHASE 9 LLQ TESTS PASSED (5/5)      \n");
	printf("========================================\n");
	return 0;
}
