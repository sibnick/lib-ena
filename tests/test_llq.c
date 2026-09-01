/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_llq.h"
#include "ena_netdev.h"
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
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf;
	uint8_t pkt_data[128];
	unsigned int cleaned = 0;

	assert(setup_test_adapter_llq(&hw, &adapter, NULL, 0) == 0);

	assert(ena_llq_negotiate(&adapter) == 0);
	assert(adapter.llq_info.supported == false);
	assert(adapter.llq_info.enabled == false);

	/* Without BAR2 the netdev TX path stays in standard host mode */
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* The last created queue (RX) used host placement, and the TX
	 * queue must not be marked as an LLQ queue */
	assert(hw.last_sq_placement == ENA_ADMIN_PLACEMENT_POLICY_HOST);
	assert(adapter.tx_rings[0]->is_llq == false);
	assert(adapter.tx_rings[0]->push_buf_virt == NULL);

	tx_buf = calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);
	memset(pkt_data, 0xAB, sizeof(pkt_data));
	tx_buf->data = pkt_data;
	tx_buf->phys_addr = 0x50001000;
	tx_buf->len = 128;

	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);
	assert(adapter.tx_rings[0]->sq_tail == 1);
	assert(mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->sq_db_offset) == 1);

	/* The descriptor must be in the host SQ ring */
	{
		const struct ena_eth_io_tx_desc *desc =
			(const struct ena_eth_io_tx_desc *)adapter.tx_rings[0]->sq_virt;

		assert((desc[0].len_ctrl & ENA_ETH_IO_TX_DESC_LENGTH_MASK) == 128);
	}

	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
	assert(ena_tx_poll_completions(adapter.tx_rings[0], 16, &cleaned) == 1);
	assert(cleaned == 1);
	assert(adapter.tx_rings[0]->sq_head == 1);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf);
	ena_admin_fini(&adapter);
	ena_netdev_free(netdev);
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

static void test_llq_tx_path_bar2(void)
{
	printf("[TEST] Running test_llq_tx_path_bar2...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf;
	uint8_t bar2_memory[65536];
	uint8_t pkt_data[128];
	const struct ena_eth_io_tx_desc *push_desc;
	const uint8_t *push_hdr;
	const struct ena_eth_io_tx_desc *host_desc;
	unsigned int cleaned = 0;
	int ret;

	mock_ena_hw_init(&hw);
	hw.dev_llq_bar_size = sizeof(bar2_memory);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	memset(bar2_memory, 0, sizeof(bar2_memory));
	memset(pkt_data, 0xAB, sizeof(pkt_data));

	/* Simulate the probe mapping the BAR2 MMIO region */
	ret = ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ret == 0);
	adapter.bar2_base = (volatile uint8_t *)bar2_memory;
	adapter.bar2_size = sizeof(bar2_memory);

	ret = ena_admin_init(&adapter, 8, 8, 8);
	assert(ret == 0);
	ret = ena_init_run(&adapter, 1500);
	assert(ret == 0);

	/* LLQ must be enabled during init when BAR2 is present */
	assert(adapter.llq_info.enabled == true);
	assert(adapter.llq_info.entry_size == 128);
	assert(adapter.llq_info.header_len == 96);

	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* The TX queue must use the BAR2 push buffer */
	assert(adapter.tx_rings[0]->is_llq == true);
	assert(adapter.tx_rings[0]->push_buf_virt != NULL);
	assert((uint8_t *)adapter.tx_rings[0]->push_buf_virt ==
	       bar2_memory + 0x1000);
	assert(adapter.tx_rings[0]->push_buf_size == 16 * 128);
	assert(adapter.tx_rings[0]->llq_entry_size == 128);
	assert(adapter.tx_rings[0]->llq_header_len == 96);

	/* The RX queue stays in host placement */
	assert(adapter.rx_rings[0]->is_llq == false);
	assert(hw.last_sq_placement == ENA_ADMIN_PLACEMENT_POLICY_HOST);

	tx_buf = calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);
	tx_buf->data = pkt_data;
	tx_buf->phys_addr = 0x50001000;
	tx_buf->len = 128;

	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);
	assert(adapter.tx_rings[0]->sq_tail == 1);
	assert(mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->sq_db_offset) == 1);

	/* The descriptor and the inline header must be in the BAR2 push
	 * buffer, not in the host SQ ring */
	push_desc = (const struct ena_eth_io_tx_desc *)(bar2_memory + 0x1000);
	assert((push_desc->len_ctrl & ENA_ETH_IO_TX_DESC_LENGTH_MASK) == 128);
	assert(push_desc->len_ctrl & ENA_ETH_IO_TX_DESC_PHASE_MASK);
	assert((push_desc->buff_addr_hi_hdr_sz >> 24) == 96);
	assert((push_desc->buff_addr_lo & 0xFFFFFFFFu) == 0x50001000);

	push_hdr = bar2_memory + 0x1000 + sizeof(*push_desc);
	assert(push_hdr[0] == 0xAB && push_hdr[95] == 0xAB);
	assert(bar2_memory[0x1000 + 112] == 0); /* Zero pad after header */

	host_desc = (const struct ena_eth_io_tx_desc *)adapter.tx_rings[0]->sq_virt;
	assert(host_desc[0].len_ctrl == 0);

	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
	assert(ena_tx_poll_completions(adapter.tx_rings[0], 16, &cleaned) == 1);
	assert(cleaned == 1);
	assert(adapter.tx_rings[0]->sq_head == 1);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf);
	ena_admin_fini(&adapter);
	ena_netdev_free(netdev);
	printf("[PASS] test_llq_tx_path_bar2 passed\n");
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
	test_llq_tx_path_bar2();
	test_llq_tx_push_direct();
	test_llq_tx_push_fallback_standard();
	test_llq_invalid_args();

	printf("========================================\n");
	printf("ALL PHASE 9 LLQ TESTS PASSED (6/6)      \n");
	printf("========================================\n");
	return 0;
}
