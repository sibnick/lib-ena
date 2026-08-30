/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_netdev.h"
#include "ena_intr.h"
#include "ena_llq.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *mock_rx_alloc_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0xA000000;
	struct uk_netbuf *nb = calloc(1, sizeof(*nb));
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x2000;
	*len_out = 9216;

	nb->phys_addr = *phys_out;
	nb->buflen = *len_out;
	return nb;
}

static void *mock_rx_alloc_fail_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	(void)arg;
	(void)phys_out;
	(void)len_out;
	return NULL;
}

static int setup_test_adapter(struct mock_ena_hw *hw, struct ena_adapter *adapter, uint16_t mtu, uint32_t max_mtu)
{
	mock_ena_hw_init(hw);
	if (max_mtu > 0)
		hw->dev_max_mtu = max_mtu;
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, hw);

	int ret = ena_device_init_scaffold(adapter, hw->bar0, sizeof(hw->bar0));
	if (ret)
		return ret;

	ret = ena_admin_init(adapter, 16, 16, 16);
	if (ret)
		return ret;

	ret = ena_init_run(adapter, mtu);
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

/* 1. Validation for EC2 t3.nano profile */
static void test_validation_t3_nano_profile(void)
{
	printf("[TEST] Running test_validation_t3_nano_profile...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_info info;

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);

	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);
	assert(netdev->ops->info_get(netdev, &info) == 0);

	/* Verify t3.nano characteristics: standard MTU 1500 and checksum offloads */
	assert(info.mtu == 1500);
	assert(info.max_rx_queues >= 1);
	assert(info.max_tx_queues >= 1);
	assert(info.features & UK_NETDEV_F_RX_CSUM);
	assert(info.features & UK_NETDEV_F_TX_CSUM);

	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_t3_nano_profile passed\n");
}

/* 2. End-to-end throughput streaming benchmark simulation */
static void test_validation_end_to_end_throughput(void)
{
	printf("[TEST] Running test_validation_end_to_end_throughput...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 32, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 32, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	const unsigned int total_packets = 500;
	unsigned int sent = 0;
	unsigned int cleaned = 0;
	struct uk_netbuf *tx_bufs = calloc(total_packets, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	while (sent < total_packets) {
		tx_bufs[sent].phys_addr = 0x1000000 + (sent * 64);
		tx_bufs[sent].len = 1460;

		assert(netdev->ops->txq_xmit(netdev, 0, &tx_bufs[sent]) == 0);
		sent++;

		/* Emulate device completions periodically */
		if (sent % 16 == 0) {
			mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 16);
			unsigned int count = 0;
			ena_tx_poll_completions(adapter.tx_rings[0], 32, &count);
			cleaned += count;
		}
	}

	if (sent > cleaned) {
		unsigned int remaining = sent - cleaned;
		mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], remaining);
		unsigned int count = 0;
		ena_tx_poll_completions(adapter.tx_rings[0], 32, &count);
		cleaned += count;
	}

	assert(sent == total_packets);
	assert(cleaned == total_packets);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_bufs);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_end_to_end_throughput passed\n");
}

/* 3. Latency roundtrip simulation */
static void test_validation_latency_roundtrip(void)
{
	printf("[TEST] Running test_validation_latency_roundtrip...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	unsigned int refilled;
	struct uk_netbuf *tx_bufs = calloc(50, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* Refill RX queue */
	assert(ena_rx_refill(adapter.rx_rings[0], 16, mock_rx_alloc_cb, NULL, &refilled) == 16);

	/* Simulate 50 ping-pong transactions */
	for (int i = 0; i < 50; i++) {
		tx_bufs[i].phys_addr = 0x2000000 + (i * 128);
		tx_bufs[i].len = 64;

		assert(netdev->ops->txq_xmit(netdev, 0, &tx_bufs[i]) == 0);

		mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
		unsigned int count = 0;
		assert(ena_tx_poll_completions(adapter.tx_rings[0], 4, &count) == 1);
		assert(count == 1);

		/* Emulate response packet on RX path */
		mock_ena_hw_emulate_rx(&hw, adapter.rx_rings[0], 1, 64, 0x12345678,
				       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

		struct uk_netbuf *rx_nb = NULL;
		assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 1);
		assert(rx_nb != NULL);
		assert(rx_nb->len == 64);
		free(rx_nb);

		/* Replenish consumed slot */
		ena_rx_refill(adapter.rx_rings[0], 1, mock_rx_alloc_cb, NULL, &refilled);
	}

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < adapter.rx_rings[0]->sq_depth; i++) {
		if (adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			free(adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	free(tx_bufs);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_latency_roundtrip passed\n");
}

/* 4. Jumbo frame MTU 9000 validation */
static void test_validation_jumbo_frames_9000(void)
{
	printf("[TEST] Running test_validation_jumbo_frames_9000...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_info info;
	unsigned int refilled;
	struct uk_netbuf *tx_buf = calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&hw, &adapter, 9000, 9000) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	assert(netdev->ops->info_get(netdev, &info) == 0);
	assert(info.mtu == 9000);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	assert(ena_rx_refill(adapter.rx_rings[0], 8, mock_rx_alloc_cb, NULL, &refilled) == 8);

	/* Transmit and receive 8960-byte payload */
	tx_buf->phys_addr = 0x3000000;
	tx_buf->len = 8960;

	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
	unsigned int count = 0;
	assert(ena_tx_poll_completions(adapter.tx_rings[0], 4, &count) == 1);
	assert(count == 1);

	mock_ena_hw_emulate_rx(&hw, adapter.rx_rings[0], 1, 8960, 0xABCDEF01,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	struct uk_netbuf *rx_nb = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 1);
	assert(rx_nb != NULL);
	assert(rx_nb->len == 8960);
	free(rx_nb);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < adapter.rx_rings[0]->sq_depth; i++) {
		if (adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			free(adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	free(tx_buf);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_jumbo_frames_9000 passed\n");
}

/* 5. Multi-queue load and queue isolation */
static void test_validation_multi_queue_load(void)
{
	printf("[TEST] Running test_validation_multi_queue_load...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_bufs = calloc(20, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	for (uint16_t q = 0; q < 2; q++) {
		assert(netdev->ops->rxq_configure(netdev, q, 16, NULL) == 0);
		assert(netdev->ops->txq_configure(netdev, q, 16, NULL) == 0);
	}

	assert(netdev->ops->dev_start(netdev) == 0);

	/* Interleave transmissions across queue 0 and queue 1 */
	for (int i = 0; i < 20; i++) {
		uint16_t q = (uint16_t)(i % 2);
		tx_bufs[i].phys_addr = 0x4000000 + (i * 256);
		tx_bufs[i].len = 1000;

		assert(netdev->ops->txq_xmit(netdev, q, &tx_bufs[i]) == 0);
	}

	/* Drain completions per queue */
	for (uint16_t q = 0; q < 2; q++) {
		mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[q], 10);
		unsigned int count = 0;
		int n = ena_tx_poll_completions(adapter.tx_rings[q], 16, &count);
		assert(n == 10);
		assert(count == 10);
	}

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_bufs);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_multi_queue_load passed\n");
}

/* 6. LLQ push mode vs standard mode comparison */
static void test_validation_llq_vs_standard_perf(void)
{
	printf("[TEST] Running test_validation_llq_vs_standard_perf...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *std_ring = NULL;
	struct ena_ring *llq_ring = NULL;
	uint8_t bar2_memory[4096];
	uint8_t header_buf[64];

	memset(bar2_memory, 0, sizeof(bar2_memory));
	memset(header_buf, 0x55, sizeof(header_buf));

	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	adapter.bar2_base = bar2_memory;
	adapter.bar2_size = sizeof(bar2_memory);

	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	assert(ena_init_run(&adapter, 1500) == 0);
	assert(ena_llq_negotiate(&adapter) == 0);
	assert(adapter.llq_info.enabled == true);

	/* Allocate standard ring */
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 16, 16, &std_ring) == 0);
	assert(ena_ring_create_hw(std_ring, 0) == 0);

	/* Allocate LLQ ring */
	assert(ena_ring_alloc(&adapter, 1, ENA_RING_TYPE_TX, 16, 16, &llq_ring) == 0);
	assert(ena_ring_create_hw(llq_ring, 1) == 0);
	llq_ring->is_llq = true;
	llq_ring->push_buf_virt = bar2_memory;

	/* Submit standard packet */
	struct ena_tx_pkt pkt_std;
	uint16_t req_id_std;
	memset(&pkt_std, 0, sizeof(pkt_std));
	pkt_std.phys_addr = 0x5000000;
	pkt_std.len = 128;
	assert(ena_tx_submit(std_ring, &pkt_std, &req_id_std) == 0);
	ena_tx_doorbell(std_ring);

	/* Submit LLQ packet */
	struct ena_tx_pkt pkt_llq;
	uint16_t req_id_llq;
	memset(&pkt_llq, 0, sizeof(pkt_llq));
	pkt_llq.phys_addr = 0x6000000;
	pkt_llq.len = 128;
	assert(ena_llq_tx_push(llq_ring, &pkt_llq, header_buf, sizeof(header_buf), &req_id_llq) == 0);
	ena_tx_doorbell(llq_ring);

	/* Verify doorbell registers updated for both rings */
	assert(std_ring->sq_tail == 1);
	assert(llq_ring->sq_tail == 1);

	assert(ena_ring_destroy_hw(std_ring) == 0);
	assert(ena_ring_destroy_hw(llq_ring) == 0);
	ena_ring_free(std_ring);
	ena_ring_free(llq_ring);
	ena_admin_fini(&adapter);
	printf("[PASS] test_validation_llq_vs_standard_perf passed\n");
}

/* 7. Stress AENQ event handling and reset recovery */
static void test_validation_stress_aenq_recovery(void)
{
	printf("[TEST] Running test_validation_stress_aenq_recovery...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);

	/* Inject link state notifications and warnings */
	mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_LINK_CHANGE, 0x1);
	mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_WARNING, 0x2);

	int events = ena_admin_aenq_poll(&adapter, 16);
	assert(events == 2);

	/* Inject fatal error notification */
	mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_FATAL_ERROR, 0x99);
	events = ena_admin_aenq_poll(&adapter, 16);
	assert(events == 1);

	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_stress_aenq_recovery passed\n");
}

/* 8. Audit Fixes Validation */
static void test_validation_audit_security_fixes(void)
{
	printf("[TEST] Running test_validation_audit_security_fixes...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *rx_ring = NULL;
	struct ena_ring *tx_ring = NULL;
	struct ena_rx_pkt rx_pkt;
	struct ena_tx_pkt tx_pkt;
	struct ena_eth_io_rx_cdesc_base *rcdesc;
	struct ena_eth_io_tx_cdesc *tcdesc;
	uint8_t buffer[512];
	uint8_t huge_hdr[128];
	uint16_t cq_idx = 0;
	uint32_t cq_db = 0;
	unsigned int cleaned = 0;

	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* Doorbell offset out of bounds rejected */
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_BAD_DB_OFFSET, 0x5000);
	assert(ena_admin_create_cq(&adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db) == -EINVAL);

	/* Valid CQ creation */
	mock_pci_clear_faults(&hw);
	assert(ena_admin_create_cq(&adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db) == 0);

	/* RX completion length exceeding buffer rejected */
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);
	assert(ena_ring_create_hw(rx_ring, 0) == 0);
	assert(ena_rx_submit_one(rx_ring, buffer, 0x1000, sizeof(buffer), NULL) == 0);

	rcdesc = (struct ena_eth_io_rx_cdesc_base *)rx_ring->cq_virt;
	memset(rcdesc, 0, sizeof(*rcdesc));
	rcdesc->req_id = ena_cpu_to_le16(0);
	rcdesc->length = ena_cpu_to_le16(1024);
	rcdesc->status = ena_cpu_to_le32((1u << ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT));
	assert(ena_rx_poll(rx_ring, &rx_pkt, 1) == 0);

	/* TX completion with unsubmitted req_id dropped */
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &tx_ring) == 0);
	assert(ena_ring_create_hw(tx_ring, 0) == 0);
	tcdesc = (struct ena_eth_io_tx_cdesc *)tx_ring->cq_virt;
	memset(tcdesc, 0, sizeof(*tcdesc));
	tcdesc->req_id = ena_cpu_to_le16(5);
	tcdesc->flags = (uint8_t)tx_ring->cq_phase;
	cleaned = 0;
	ena_tx_poll_completions(tx_ring, 1, &cleaned);
	assert(cleaned == 0);
	assert(tx_ring->free_req_count == 8);

	/* LLQ header length > 96 rejected */
	memset(&tx_pkt, 0, sizeof(tx_pkt));
	tx_pkt.len = 200;
	assert(ena_llq_tx_push(tx_ring, &tx_pkt, huge_hdr, 110, NULL) == -EINVAL);

	ena_ring_destroy_hw(rx_ring);
	ena_ring_free(rx_ring);
	ena_ring_destroy_hw(tx_ring);
	ena_ring_free(tx_ring);
	ena_admin_fini(&adapter);

	printf("[PASS] test_validation_audit_security_fixes passed\n");
}

/* 9. Boundary: Unaligned doorbell offsets */
static void test_validation_boundary_unaligned_doorbell_offsets(void)
{
	printf("[TEST] Running test_validation_boundary_unaligned_doorbell_offsets...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	uint16_t cq_idx = 0;
	uint32_t cq_db = 0;
	uint16_t sq_idx = 0;
	uint32_t sq_db = 0;

	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* Inject unaligned CQ doorbell offset */
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET, 0x30);
	assert(ena_admin_create_cq(&adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db) == -EINVAL);

	/* Clear and create valid CQ */
	mock_pci_clear_faults(&hw);
	assert(ena_admin_create_cq(&adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db) == 0);

	/* Inject unaligned SQ doorbell offset */
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET, 0x2C);
	assert(ena_admin_create_sq(&adapter, 8, 0x2000, 0, cq_idx, 1, &sq_idx, &sq_db) == -EINVAL);

	mock_pci_clear_faults(&hw);
	ena_admin_fini(&adapter);
	printf("[PASS] test_validation_boundary_unaligned_doorbell_offsets passed\n");
}

/* 10. Boundary: Queue ID bounds */
static void test_validation_boundary_queue_id_bounds(void)
{
	printf("[TEST] Running test_validation_boundary_queue_id_bounds...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	/* Out-of-bounds queue configurations */
	assert(netdev->ops->rxq_configure(netdev, 2, 8, NULL) == -EINVAL);
	assert(netdev->ops->rxq_configure(netdev, 10, 8, NULL) == -EINVAL);
	assert(netdev->ops->txq_configure(netdev, 2, 8, NULL) == -EINVAL);
	assert(netdev->ops->txq_configure(netdev, 99, 8, NULL) == -EINVAL);

	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->rxq_configure(netdev, 1, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 1, 8, NULL) == 0);

	assert(netdev->ops->dev_start(netdev) == 0);

	/* Out-of-bounds xmit and recv */
	tx_buf->len = 64;
	assert(netdev->ops->txq_xmit(netdev, 2, tx_buf) == -EINVAL);
	assert(netdev->ops->txq_xmit(netdev, 5, tx_buf) == -EINVAL);
	assert(netdev->ops->rxq_recv(netdev, 2, &rx_buf) == -EINVAL);
	assert(netdev->ops->rxq_recv(netdev, 8, &rx_buf) == -EINVAL);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_boundary_queue_id_bounds passed\n");
}

/* 11. Boundary: LLQ header length boundary tests */
static void test_validation_boundary_llq_header_lengths(void)
{
	printf("[TEST] Running test_validation_boundary_llq_header_lengths...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint8_t hdr_buf[256];
	uint16_t req_id;

	memset(hdr_buf, 0xAA, sizeof(hdr_buf));
	memset(&pkt, 0, sizeof(pkt));
	pkt.len = 256;

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);

	/* Lengths above 96 must fail with -EINVAL */
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 97, &req_id) == -EINVAL);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 128, &req_id) == -EINVAL);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 200, &req_id) == -EINVAL);

	/* Length 0 and 96 must pass boundary check */
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 0, &req_id) == 0);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 96, &req_id) == 0);

	ena_ring_free(ring);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_boundary_llq_header_lengths passed\n");
}

/* 12. Boundary: dev_stop queue teardown and restart */
static void test_validation_boundary_dev_stop_teardown(void)
{
	printf("[TEST] Running test_validation_boundary_dev_stop_teardown...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);

	/* Cycle 1: Start and transmit */
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(ena_rx_refill(adapter.rx_rings[0], 4, mock_rx_alloc_cb, NULL, &refilled) == 4);

	tx_buf->phys_addr = 0x7000000;
	tx_buf->len = 128;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	/* Stop device */
	assert(netdev->ops->dev_stop(netdev) == 0);
	assert(netdev->state == UK_NETDEV_STOPPED);

	/* Transmission when stopped must return -EAGAIN */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == -EAGAIN);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == -EAGAIN);

	/* Cycle 2: Restart device and transmit */
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < adapter.rx_rings[0]->sq_depth; i++) {
		if (adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			free(adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	free(tx_buf);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_boundary_dev_stop_teardown passed\n");
}

/* 13. Boundary: RX allocation failures */
static void test_validation_boundary_rx_allocation_failures(void)
{
	printf("[TEST] Running test_validation_boundary_rx_allocation_failures...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct ena_ring *rx_ring = NULL;
	unsigned int refilled = 0;

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);
	assert(ena_ring_create_hw(rx_ring, 0) == 0);

	/* Allocation failure callback returns 0 refilled packets */
	int count = ena_rx_refill(rx_ring, 4, mock_rx_alloc_fail_cb, NULL, &refilled);
	assert(count == 0);
	assert(refilled == 0);
	assert(rx_ring->free_req_count == 8);

	/* Subsequent valid refill succeeds */
	count = ena_rx_refill(rx_ring, 4, mock_rx_alloc_cb, NULL, &refilled);
	assert(count == 4);
	assert(refilled == 4);
	assert(rx_ring->free_req_count == 4);

	for (int i = 0; i < rx_ring->sq_depth; i++) {
		if (rx_ring->buffers.rx_bufs[i].netbuf) {
			free(rx_ring->buffers.rx_bufs[i].netbuf);
			rx_ring->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	ena_ring_destroy_hw(rx_ring);
	ena_ring_free(rx_ring);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_boundary_rx_allocation_failures passed\n");
}

/* 14. Boundary: Post-timeout admin recovery */
static void test_validation_boundary_post_timeout_admin_recovery(void)
{
	printf("[TEST] Running test_validation_boundary_post_timeout_admin_recovery...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* Inject admin hang fault */
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_ADMIN_HANG, 0);

	int ret = ena_init_get_device_attributes(&adapter);
	assert(ret == -ETIMEDOUT || ret == -EIO);
	assert(adapter.state == ENA_STATE_ERROR);

	/* Clear hang and re-init admin queue to recover */
	mock_pci_clear_faults(&hw);
	ena_admin_fini(&adapter);
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	ret = ena_init_get_device_attributes(&adapter);
	assert(ret == 0);
	assert(adapter.max_mtu == 1500);

	ena_admin_fini(&adapter);
	printf("[PASS] test_validation_boundary_post_timeout_admin_recovery passed\n");
}

/* 15. Boundary: Corrupted ACQ completions */
static void test_validation_boundary_corrupted_acq_completions(void)
{
	printf("[TEST] Running test_validation_boundary_corrupted_acq_completions...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;

	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* Inject illegal admin status response */
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_ADMIN_STATUS, ENA_ADMIN_ILLEGAL_PARAMETER);
	int ret = ena_init_get_device_attributes(&adapter);
	assert(ret == -(int)ENA_ADMIN_ILLEGAL_PARAMETER);

	/* Inject bad command ID response */
	mock_pci_clear_faults(&hw);
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_BAD_CMD_ID, 0x0EAD);
	ret = ena_init_get_device_attributes(&adapter);
	assert(ret == -ETIMEDOUT || ret == -EIO || ret != 0);

	/* Clear faults and re-init to verify normal operation */
	mock_pci_clear_faults(&hw);
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	ena_admin_fini(&adapter);
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	ret = ena_init_get_device_attributes(&adapter);
	assert(ret == 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_validation_boundary_corrupted_acq_completions passed\n");
}

/* 16. Concurrency stress across multiple queues */
static void test_validation_concurrency_stress_queues(void)
{
	printf("[TEST] Running test_validation_concurrency_stress_queues...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	const unsigned int total_pkts = 80;
	struct uk_netbuf *tx_bufs = calloc(total_pkts, sizeof(*tx_bufs));
	unsigned int refilled;
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&hw, &adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 4;
	conf.nb_tx_queues = 4;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	for (uint16_t q = 0; q < 4; q++) {
		assert(netdev->ops->rxq_configure(netdev, q, 32, NULL) == 0);
		assert(netdev->ops->txq_configure(netdev, q, 32, NULL) == 0);
	}

	assert(netdev->ops->dev_start(netdev) == 0);

	/* Refill all 4 RX queues */
	for (uint16_t q = 0; q < 4; q++)
		assert(ena_rx_refill(adapter.rx_rings[q], 8, mock_rx_alloc_cb, NULL, &refilled) == 8);

	uint8_t raw_payloads[80][256];
	memset(raw_payloads, 0, sizeof(raw_payloads));
	for (unsigned int i = 0; i < total_pkts; i++) {
		raw_payloads[i][12] = 0x08;
		raw_payloads[i][13] = 0x00;
		raw_payloads[i][23] = 6;
	}

	/* Interleave TX bursts and AENQ notifications */
	for (unsigned int i = 0; i < total_pkts; i++) {
		uint16_t q = (uint16_t)(i % 4);
		tx_bufs[i].data = raw_payloads[i];
		tx_bufs[i].phys_addr = 0x8000000 + (i * 128);
		tx_bufs[i].len = 256;
		assert(netdev->ops->txq_xmit(netdev, q, &tx_bufs[i]) == 0);

		if (i % 20 == 19)
			mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_WARNING, (uint16_t)i);
	}

	/* Complete and poll across all queues */
	for (uint16_t q = 0; q < 4; q++) {
		mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[q], 20);
		unsigned int count = 0;
		int n = ena_tx_poll_completions(adapter.tx_rings[q], 20, &count);
		assert(n == 20);
		assert(count == 20);
	}

	/* Drain AENQ events */
	int events = ena_admin_aenq_poll(&adapter, 16);
	assert(events >= 4);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (uint16_t q = 0; q < 4; q++) {
		for (int i = 0; i < adapter.rx_rings[q]->sq_depth; i++) {
			if (adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf) {
				free(adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf);
				adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf = NULL;
			}
		}
	}
	free(tx_bufs);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);
	printf("[PASS] test_validation_concurrency_stress_queues passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 10 Validation\n");
	printf("========================================\n");

	test_validation_t3_nano_profile();
	test_validation_end_to_end_throughput();
	test_validation_latency_roundtrip();
	test_validation_jumbo_frames_9000();
	test_validation_multi_queue_load();
	test_validation_llq_vs_standard_perf();
	test_validation_stress_aenq_recovery();
	test_validation_audit_security_fixes();
	test_validation_boundary_unaligned_doorbell_offsets();
	test_validation_boundary_queue_id_bounds();
	test_validation_boundary_llq_header_lengths();
	test_validation_boundary_dev_stop_teardown();
	test_validation_boundary_rx_allocation_failures();
	test_validation_boundary_post_timeout_admin_recovery();
	test_validation_boundary_corrupted_acq_completions();
	test_validation_concurrency_stress_queues();

	printf("========================================\n");
	printf("ALL PHASE 10 VALIDATION TESTS PASSED (16/16)\n");
	printf("========================================\n");
	return 0;
}
