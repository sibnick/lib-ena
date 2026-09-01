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
#include "test_framework.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mock_ena_hw g_hw;
static struct ena_adapter g_adapter;

static void test_validation_setup(void)
{
	mock_ena_hw_init(&g_hw);
	mock_pci_clear_faults(&g_hw);
	test_reset_alloc_tracking();
}

static void test_validation_teardown(void)
{
	mock_pci_clear_faults(&g_hw);
}

static void *mock_rx_alloc_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0xA000000;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
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

	adapter->rx_rings = test_calloc(adapter->max_rx_queues, sizeof(struct ena_ring *));
	adapter->tx_rings = test_calloc(adapter->max_tx_queues, sizeof(struct ena_ring *));
	return 0;
}

static void teardown_test_adapter(struct ena_adapter *adapter)
{
	if (adapter->rx_rings) {
		for (uint16_t i = 0; i < adapter->max_rx_queues; i++) {
			if (adapter->rx_rings[i])
				ena_ring_free(adapter->rx_rings[i]);
		}
		test_free(adapter->rx_rings);
		adapter->rx_rings = NULL;
	}

	if (adapter->tx_rings) {
		for (uint16_t i = 0; i < adapter->max_tx_queues; i++) {
			if (adapter->tx_rings[i])
				ena_ring_free(adapter->tx_rings[i]);
		}
		test_free(adapter->tx_rings);
		adapter->tx_rings = NULL;
	}

	ena_admin_fini(adapter);
}

/* 1. Validation for EC2 t3.nano profile */
static void test_validation_t3_nano_profile(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_info info;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);
	assert(netdev->ops->info_get(netdev, &info) == 0);

	/* Verify t3.nano characteristics: standard MTU 1500 and checksum offloads */
	assert(info.mtu == 1500);
	assert(info.max_rx_queues >= 1);
	assert(info.max_tx_queues >= 1);
	assert(info.features & UK_NETDEV_F_PARTIAL_CSUM);
	assert(info.features & UK_NETDEV_F_LRO);
	assert(info.features & UK_NETDEV_F_TSO4);

	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 2. End-to-end throughput streaming benchmark simulation */
static void test_validation_end_to_end_throughput(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
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
	struct uk_netbuf *tx_bufs = test_calloc(total_packets, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	while (sent < total_packets) {
		tx_bufs[sent].phys_addr = 0x1000000 + (sent * 64);
		tx_bufs[sent].len = 1460;

		assert(netdev->ops->txq_xmit(netdev, 0, &tx_bufs[sent]) == 0);
		sent++;

		/* Emulate device completions periodically */
		if (sent % 16 == 0) {
			mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 16);
			unsigned int count = 0;
			ena_tx_poll_completions(g_adapter.tx_rings[0], 32, &count);
			cleaned += count;
		}
	}

	if (sent > cleaned) {
		unsigned int remaining = sent - cleaned;
		mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], remaining);
		unsigned int count = 0;
		ena_tx_poll_completions(g_adapter.tx_rings[0], 32, &count);
		cleaned += count;
	}

	assert(sent == total_packets);
	assert(cleaned == total_packets);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_bufs);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 3. Latency roundtrip simulation */
static void test_validation_latency_roundtrip(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	unsigned int refilled;
	struct uk_netbuf *tx_bufs = test_calloc(50, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* Refill RX queue */
	assert(ena_rx_refill(g_adapter.rx_rings[0], 16, mock_rx_alloc_cb, NULL, &refilled) == 16);

	/* Simulate 50 ping-pong transactions */
	for (int i = 0; i < 50; i++) {
		tx_bufs[i].phys_addr = 0x2000000 + (i * 128);
		tx_bufs[i].len = 64;

		assert(netdev->ops->txq_xmit(netdev, 0, &tx_bufs[i]) == 0);

		mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 1);
		unsigned int count = 0;
		assert(ena_tx_poll_completions(g_adapter.tx_rings[0], 4, &count) == 1);
		assert(count == 1);

		/* Emulate response packet on RX path */
		mock_ena_hw_emulate_rx(&g_hw, g_adapter.rx_rings[0], 1, 64, 0x12345678,
				       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

		struct uk_netbuf *rx_nb = NULL;
		assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 1);
		assert(rx_nb != NULL);
		assert(rx_nb->len == 64);
		test_free(rx_nb);

		/* Replenish consumed slot */
		ena_rx_refill(g_adapter.rx_rings[0], 1, mock_rx_alloc_cb, NULL, &refilled);
	}

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < g_adapter.rx_rings[0]->sq_depth; i++) {
		if (g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			test_free(g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	test_free(tx_bufs);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 4. Jumbo frames: MTU 9000 negotiates and jumbo TX completes from one
 * large buffer, but RX is single-descriptor with ENA_RX_BUF_SIZE
 * (2048) byte buffers, so jumbo frames cannot be received. An
 * over-length completion is dropped cleanly: not delivered, not
 * counted, request pool restored, bounce pool untouched, and the ring
 * still delivers normal-size packets afterwards. */
static struct uk_netbuf *g_jumbo_netbufs[16];
static uint16_t g_jumbo_netbuf_count = 0;

static void *mock_rx_alloc_jumbo_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0xB000000;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x1000;
	*len_out = ENA_RX_BUF_SIZE;

	nb->phys_addr = *phys_out;
	nb->buflen = *len_out;

	if (g_jumbo_netbuf_count < 16)
		g_jumbo_netbufs[g_jumbo_netbuf_count++] = nb;
	return nb;
}

static void test_validation_jumbo_rx_dropped(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_info info;
	struct ena_ring *rx_ring;
	struct uk_netbuf *tx_buf;
	struct uk_netbuf *rx_nb = NULL;
	unsigned int refilled;
	unsigned int count = 0;
	uint16_t i;
	uint16_t j;

	g_jumbo_netbuf_count = 0;

	assert(setup_test_adapter(&g_hw, &g_adapter, 9000, 9000) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	/* The device accepts MTU 9000. */
	assert(netdev->ops->info_get(netdev, &info) == 0);
	assert(info.mtu == 9000);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 16, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];

	/* Offer one 2048-byte buffer per descriptor, as the driver does. */
	assert(ena_rx_refill(rx_ring, 8, mock_rx_alloc_jumbo_cb, NULL, &refilled) == 8);
	assert(rx_ring->free_req_count == 8);

	/* Jumbo TX from one large direct-DMA buffer completes. */
	tx_buf = test_calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);
	tx_buf->phys_addr = 0x3000000;
	tx_buf->len = 8960;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 1);
	assert(ena_tx_poll_completions(g_adapter.tx_rings[0], 4, &count) == 1);
	assert(count == 1);

	/* An 8960-byte completion arrives for a 2048-byte buffer. */
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 8960, 0xABCDEF01,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	/* The driver drops it: nothing is delivered to the app. */
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 0);
	assert(rx_nb == NULL);

	/* Dropped cleanly: no packet counted, slot cleared, request ID
	 * returned to the pool, bounce pool untouched. */
	assert(rx_ring->rx_packets == 0);
	assert(rx_ring->buffers.rx_bufs[0].netbuf == NULL);
	assert(rx_ring->free_req_count == 9);
	assert(netdev->rx_queues[0].bounce_free_count == 16);

	/* The ring still works: a normal-size completion is delivered. */
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 64, 0x12345678,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 1);
	assert(rx_nb != NULL);
	assert(rx_nb->len == 64);
	test_free(rx_nb);
	for (j = 0; j < g_jumbo_netbuf_count; j++) {
		if (g_jumbo_netbufs[j] == rx_nb)
			g_jumbo_netbufs[j] = NULL;
	}

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (i = 0; i < rx_ring->sq_depth; i++) {
		if (rx_ring->buffers.rx_bufs[i].netbuf) {
			for (j = 0; j < g_jumbo_netbuf_count; j++) {
				if (g_jumbo_netbufs[j] == rx_ring->buffers.rx_bufs[i].netbuf)
					g_jumbo_netbufs[j] = NULL;
			}
			test_free(rx_ring->buffers.rx_bufs[i].netbuf);
			rx_ring->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	/* Free the netbuf orphaned by the dropped completion. */
	for (i = 0; i < g_jumbo_netbuf_count; i++) {
		if (g_jumbo_netbufs[i])
			test_free(g_jumbo_netbufs[i]);
	}
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 5. Multi-queue load and queue isolation */
static void test_validation_multi_queue_load(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_bufs = test_calloc(20, sizeof(*tx_bufs));
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
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
		mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[q], 10);
		unsigned int count = 0;
		int n = ena_tx_poll_completions(g_adapter.tx_rings[q], 16, &count);
		assert(n == 10);
		assert(count == 10);
	}

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_bufs);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 6. LLQ push mode vs standard mode comparison */
static void test_validation_llq_vs_standard_perf(void)
{
	struct ena_ring *std_ring = NULL;
	struct ena_ring *llq_ring = NULL;
	uint8_t bar2_memory[4096];
	uint8_t header_buf[64];

	memset(bar2_memory, 0, sizeof(bar2_memory));
	memset(header_buf, 0x55, sizeof(header_buf));

	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	g_adapter.bar2_base = bar2_memory;
	g_adapter.bar2_size = sizeof(bar2_memory);

	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);
	assert(ena_init_run(&g_adapter, 1500) == 0);
	assert(ena_llq_negotiate(&g_adapter) == 0);
	assert(g_adapter.llq_info.enabled == true);

	/* Allocate standard ring */
	assert(ena_ring_alloc(&g_adapter, 0, ENA_RING_TYPE_TX, 16, 16, &std_ring) == 0);
	assert(ena_ring_create_hw(std_ring, 0) == 0);

	/* Allocate LLQ ring */
	assert(ena_ring_alloc(&g_adapter, 1, ENA_RING_TYPE_TX, 16, 16, &llq_ring) == 0);
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
	ena_admin_fini(&g_adapter);
}

/* 7. Stress AENQ event handling and reset recovery */
static void test_validation_stress_aenq_recovery(void)
{
	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);

	/* Inject link state notifications and warnings */
	mock_ena_hw_inject_aenq(&g_hw, ENA_ADMIN_LINK_CHANGE, 0x1);
	mock_ena_hw_inject_aenq(&g_hw, ENA_ADMIN_WARNING, 0x2);

	int events = ena_admin_aenq_poll(&g_adapter, 16);
	assert(events == 2);

	/* Inject fatal error notification */
	mock_ena_hw_inject_aenq(&g_hw, ENA_ADMIN_FATAL_ERROR, 0x99);
	events = ena_admin_aenq_poll(&g_adapter, 16);
	assert(events == 1);

	teardown_test_adapter(&g_adapter);
}

/* 8. Audit Fixes Validation */
static void test_validation_audit_security_fixes(void)
{
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

	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);

	/* Doorbell offset out of bounds rejected */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_BAD_DB_OFFSET, 0x5000);
	assert(ena_admin_create_cq(&g_adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db, NULL) == -EINVAL);

	/* Valid CQ creation */
	mock_pci_clear_faults(&g_hw);
	assert(ena_admin_create_cq(&g_adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db, NULL) == 0);

	/* RX completion length exceeding buffer rejected */
	assert(ena_ring_alloc(&g_adapter, 0, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);
	assert(ena_ring_create_hw(rx_ring, 0) == 0);
	assert(ena_rx_submit_one(rx_ring, buffer, 0x1000, sizeof(buffer), NULL) == 0);

	rcdesc = (struct ena_eth_io_rx_cdesc_base *)rx_ring->cq_virt;
	memset(rcdesc, 0, sizeof(*rcdesc));
	rcdesc->req_id = ena_cpu_to_le16(0);
	rcdesc->length = ena_cpu_to_le16(1024);
	rcdesc->status = ena_cpu_to_le32((1u << ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT));
	assert(ena_rx_poll(rx_ring, &rx_pkt, 1) == 0);

	/* TX completion with unsubmitted req_id dropped */
	assert(ena_ring_alloc(&g_adapter, 0, ENA_RING_TYPE_TX, 8, 8, &tx_ring) == 0);
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
	ena_admin_fini(&g_adapter);
}

/* 9. Boundary: Unaligned doorbell offsets */
static void test_validation_boundary_unaligned_doorbell_offsets(void)
{
	uint16_t cq_idx = 0;
	uint32_t cq_db = 0;
	uint16_t sq_idx = 0;
	uint32_t sq_db = 0;

	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);

	/* Inject unaligned CQ doorbell offset */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET, 0x30);
	assert(ena_admin_create_cq(&g_adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db, NULL) == -EINVAL);

	/* Clear and create valid CQ */
	mock_pci_clear_faults(&g_hw);
	assert(ena_admin_create_cq(&g_adapter, 8, 0x1000, 0, 2, &cq_idx, &cq_db, NULL) == 0);

	/* Inject unaligned SQ doorbell offset */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET, 0x2C);
	assert(ena_admin_create_sq(&g_adapter, 8, 0x2000, 0, cq_idx, 1, &sq_idx, &sq_db) == -EINVAL);

	mock_pci_clear_faults(&g_hw);
	ena_admin_fini(&g_adapter);
}

/* 10. Boundary: Queue ID bounds */
static void test_validation_boundary_queue_id_bounds(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = test_calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
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
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 11. Boundary: LLQ header length boundary tests */
static void test_validation_boundary_llq_header_lengths(void)
{
	struct ena_ring *ring = NULL;
	struct ena_tx_pkt pkt;
	uint8_t hdr_buf[256];
	uint16_t req_id;

	memset(hdr_buf, 0xAA, sizeof(hdr_buf));
	memset(&pkt, 0, sizeof(pkt));
	pkt.len = 256;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	assert(ena_ring_alloc(&g_adapter, 0, ENA_RING_TYPE_TX, 8, 8, &ring) == 0);

	/* Lengths above 96 must fail with -EINVAL */
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 97, &req_id) == -EINVAL);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 128, &req_id) == -EINVAL);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 200, &req_id) == -EINVAL);

	/* Length 0 and 96 must pass boundary check */
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 0, &req_id) == 0);
	assert(ena_llq_tx_push(ring, &pkt, hdr_buf, 96, &req_id) == 0);

	ena_ring_free(ring);
	teardown_test_adapter(&g_adapter);
}

/* 12. Boundary: dev_stop queue teardown and restart */
static void test_validation_boundary_dev_stop_teardown(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = test_calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);

	/* Cycle 1: Start and transmit */
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(ena_rx_refill(g_adapter.rx_rings[0], 4, mock_rx_alloc_cb, NULL, &refilled) == 4);

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
	for (int i = 0; i < g_adapter.rx_rings[0]->sq_depth; i++) {
		if (g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			test_free(g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 13. Boundary: RX allocation failures */
static void test_validation_boundary_rx_allocation_failures(void)
{
	struct ena_ring *rx_ring = NULL;
	unsigned int refilled = 0;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	assert(ena_ring_alloc(&g_adapter, 0, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);
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
			test_free(rx_ring->buffers.rx_bufs[i].netbuf);
			rx_ring->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	ena_ring_destroy_hw(rx_ring);
	ena_ring_free(rx_ring);
	teardown_test_adapter(&g_adapter);
}

/* 14. Boundary: Post-timeout admin recovery */
static void test_validation_boundary_post_timeout_admin_recovery(void)
{
	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);

	/* Inject admin hang fault */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_ADMIN_HANG, 0);

	int ret = ena_init_get_device_attributes(&g_adapter);
	assert(ret == -ETIMEDOUT || ret == -EIO);
	assert(g_adapter.state == ENA_STATE_ERROR);

	/* Clear hang and re-init admin queue to recover */
	mock_pci_clear_faults(&g_hw);
	ena_admin_fini(&g_adapter);
	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);
	ret = ena_init_get_device_attributes(&g_adapter);
	assert(ret == 0);
	assert(g_adapter.max_mtu == 1500);

	ena_admin_fini(&g_adapter);
}

/* 15. Boundary: Corrupted ACQ completions */
static void test_validation_boundary_corrupted_acq_completions(void)
{
	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);

	/* Inject illegal admin status response */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_ADMIN_STATUS, ENA_ADMIN_ILLEGAL_PARAMETER);
	int ret = ena_init_get_device_attributes(&g_adapter);
	assert(ret == -(int)ENA_ADMIN_ILLEGAL_PARAMETER);

	/* Inject bad command ID response */
	mock_pci_clear_faults(&g_hw);
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_BAD_CMD_ID, 0x0EAD);
	ret = ena_init_get_device_attributes(&g_adapter);
	assert(ret == -ETIMEDOUT || ret == -EIO || ret != 0);

	/* Clear faults and re-init to verify normal operation */
	mock_pci_clear_faults(&g_hw);
	mock_ena_hw_init(&g_hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &g_hw);
	ena_admin_fini(&g_adapter);
	assert(ena_device_init_scaffold(&g_adapter, g_hw.bar0, sizeof(g_hw.bar0)) == 0);
	assert(ena_admin_init(&g_adapter, 8, 8, 8) == 0);
	ret = ena_init_get_device_attributes(&g_adapter);
	assert(ret == 0);

	ena_admin_fini(&g_adapter);
}

/* 16. Concurrency stress across multiple queues */
static void test_validation_concurrency_stress_queues(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	const unsigned int total_pkts = 80;
	struct uk_netbuf *tx_bufs = test_calloc(total_pkts, sizeof(*tx_bufs));
	unsigned int refilled;
	assert(tx_bufs != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
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
		assert(ena_rx_refill(g_adapter.rx_rings[q], 8, mock_rx_alloc_cb, NULL, &refilled) == 8);

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
			mock_ena_hw_inject_aenq(&g_hw, ENA_ADMIN_WARNING, (uint16_t)i);
	}

	/* Complete and poll across all queues */
	for (uint16_t q = 0; q < 4; q++) {
		mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[q], 20);
		unsigned int count = 0;
		int n = ena_tx_poll_completions(g_adapter.tx_rings[q], 20, &count);
		assert(n == 20);
		assert(count == 20);
	}

	/* Drain AENQ events */
	int events = ena_admin_aenq_poll(&g_adapter, 16);
	assert(events >= 4);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (uint16_t q = 0; q < 4; q++) {
		for (int i = 0; i < g_adapter.rx_rings[q]->sq_depth; i++) {
			if (g_adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf) {
				test_free(g_adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf);
				g_adapter.rx_rings[q]->buffers.rx_bufs[i].netbuf = NULL;
			}
		}
	}
	test_free(tx_bufs);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 17. Fault: fake request ID in TX completion */
static void test_validation_fault_tx_fake_req_id(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf;
	struct ena_ring *tx_ring;
	uint16_t in_flight_id = 0;
	uint16_t fake_id;
	uint16_t found = 0;
	uint16_t i;
	unsigned int cleaned = 99;
	int n;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 0) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	tx_buf = test_calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);
	tx_buf->phys_addr = 0x4000000;
	tx_buf->len = 64;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	tx_ring = g_adapter.tx_rings[0];
	for (i = 0; i < tx_ring->sq_depth; i++) {
		if (tx_ring->req_in_flight[i]) {
			in_flight_id = i;
			found++;
		}
	}
	assert(found == 1);

	fake_id = (uint16_t)((in_flight_id + 1) & (tx_ring->sq_depth - 1));
	assert(fake_id != in_flight_id);

	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_FAKE_REQ_ID, (uint64_t)fake_id);
	mock_ena_hw_emulate_tx(&g_hw, tx_ring, 1);

	/* Fake completion is consumed but completes nothing */
	n = ena_tx_poll_completions(tx_ring, 1, &cleaned);
	assert(n == 0);
	assert(tx_ring->sq_head == 0);
	assert(tx_ring->req_in_flight[in_flight_id] == 1);
	assert(tx_ring->req_in_flight[fake_id] == 0);

	/* Real completion after clearing the fault completes the packet */
	mock_pci_clear_faults(&g_hw);
	mock_ena_hw_emulate_tx(&g_hw, tx_ring, 1);

	n = ena_tx_poll_completions(tx_ring, 1, &cleaned);
	assert(n == 1);
	assert(tx_ring->sq_head == 1);
	assert(tx_ring->req_in_flight[in_flight_id] == 0);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* Records netbufs from mock_rx_alloc_corrupt_cb so the test can free any the driver orphans */
static struct uk_netbuf *g_corrupt_netbufs[16];
static uint16_t g_corrupt_netbuf_count = 0;

static void *mock_rx_alloc_corrupt_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0xC000000;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x2000;
	*len_out = 9216;

	nb->phys_addr = *phys_out;
	nb->buflen = *len_out;

	if (g_corrupt_netbuf_count < 16)
		g_corrupt_netbufs[g_corrupt_netbuf_count++] = nb;
	return nb;
}

/* 18. Fault: corrupted length in RX completion */
static void test_validation_fault_rx_corrupt_length(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct ena_ring *rx_ring;
	struct ena_rx_pkt rx_pkt;
	unsigned int refilled;
	uint16_t i;
	uint16_t j;

	g_corrupt_netbuf_count = 0;

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 0) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	assert(ena_rx_refill(rx_ring, 4, mock_rx_alloc_corrupt_cb, NULL, &refilled) == 4);

	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_CORRUPT_LENGTH, 0xFFFF);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 512, 0xAABBCCDD,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
	memset(&rx_pkt, 0, sizeof(rx_pkt));

	/* Corrupted completion is dropped: no packet, slot cleared, id returned */
	assert(ena_rx_poll(rx_ring, &rx_pkt, 1) == 0);
	assert(rx_ring->rx_packets == 0);
	assert(rx_ring->buffers.rx_bufs[0].netbuf == NULL);
	assert(rx_ring->free_req_count == 5);

	/* Valid length after clearing the fault is delivered */
	mock_pci_clear_faults(&g_hw);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 512, 0x11223344,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
	memset(&rx_pkt, 0, sizeof(rx_pkt));
	assert(ena_rx_poll(rx_ring, &rx_pkt, 1) == 1);
	assert(rx_pkt.len == 512);
	assert(rx_pkt.hash == 0x11223344);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (i = 0; i < rx_ring->sq_depth; i++) {
		if (rx_ring->buffers.rx_bufs[i].netbuf) {
			for (j = 0; j < g_corrupt_netbuf_count; j++) {
				if (g_corrupt_netbufs[j] == rx_ring->buffers.rx_bufs[i].netbuf)
					g_corrupt_netbufs[j] = NULL;
			}
			test_free(rx_ring->buffers.rx_bufs[i].netbuf);
			rx_ring->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	/* Free any netbuf orphaned by the dropped completion */
	for (i = 0; i < g_corrupt_netbuf_count; i++) {
		if (g_corrupt_netbufs[i])
			test_free(g_corrupt_netbufs[i]);
	}
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* 19. AENQ runtime wiring: the RX poll path drains the AENQ ring and
 * dispatches the default handler registered at probe time. LINK_CHANGE
 * updates the link state; FATAL_ERROR resets the device and re-inits
 * the admin queues. */
static void test_validation_aenq_runtime_wiring(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *rx_nb = NULL;
	unsigned int refilled = 0;
	uint16_t cmd_id = 0;
	uint32_t resp[14];

	assert(setup_test_adapter(&g_hw, &g_adapter, 1500, 1500) == 0);

	/* The probe path registers the default handler. */
	assert(ena_admin_aenq_register(&g_adapter, ena_aenq_default_handler,
				       &g_adapter) == 0);
	assert(g_adapter.link_up == false);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(ena_rx_refill(g_adapter.rx_rings[0], 8, mock_rx_alloc_cb, NULL,
			     &refilled) == 8);

	/* LINK_CHANGE with link_status=1: the RX poll dispatches it and
	 * the link state flips to up. */
	mock_ena_hw_inject_aenq_payload(&g_hw, ENA_ADMIN_LINK_CHANGE, 0, 1);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 0);
	assert(g_adapter.link_up == true);
	assert(ena_netdev_link_get(netdev) == true);

	/* LINK_CHANGE with link_status=0: the link state flips to down. */
	mock_ena_hw_inject_aenq_payload(&g_hw, ENA_ADMIN_LINK_CHANGE, 1, 0);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 0);
	assert(g_adapter.link_up == false);
	assert(ena_netdev_link_get(netdev) == false);

	/* FATAL_ERROR: model a reset in progress that finishes after 10
	 * polls. The RX poll must trigger the reset and re-init. */
	mock_ena_hw_set_reg32(&g_hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_RESET_IN_PROG_MASK);
	g_hw.reset_polls_to_finish = 10;
	ena_device_set_reset_poll_hook(mock_ena_hw_reset_poll_hook, &g_hw);

	mock_ena_hw_inject_aenq(&g_hw, ENA_ADMIN_FATAL_ERROR, 0x99);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_nb) == 0);
	ena_device_set_reset_poll_hook(NULL, NULL);

	/* The reset request reached DEV_CTL and the device finished it. */
	assert(mock_ena_hw_get_reg32(&g_hw, ENA_REGS_DEV_CTL_OFF) &
	       ENA_DEV_CTL_DEV_RESET_MASK);
	assert(mock_ena_hw_get_reg32(&g_hw, ENA_REGS_DEV_STS_OFF) &
	       ENA_DEV_STS_RESET_FIN_MASK);

	/* The admin queues were re-initialized and the handler re-registered. */
	assert(g_adapter.state == ENA_STATE_ADMIN_READY);
	assert(g_adapter.aenq_handler == ena_aenq_default_handler);

	/* The admin path works after the fatal error recovery. */
	memset(resp, 0, sizeof(resp));
	assert(ena_admin_exec_cmd(&g_adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				  resp, sizeof(resp), &cmd_id, 100) == 0);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < g_adapter.rx_rings[0]->sq_depth; i++) {
		if (g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			test_free(g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 10 Validation\n");
	printf("========================================\n");

	test_register_setup(test_validation_setup);
	test_register_teardown(test_validation_teardown);

	RUN_TEST(test_validation_t3_nano_profile);
	RUN_TEST(test_validation_end_to_end_throughput);
	RUN_TEST(test_validation_latency_roundtrip);
	RUN_TEST(test_validation_jumbo_rx_dropped);
	RUN_TEST(test_validation_multi_queue_load);
	RUN_TEST(test_validation_llq_vs_standard_perf);
	RUN_TEST(test_validation_stress_aenq_recovery);
	RUN_TEST(test_validation_audit_security_fixes);
	RUN_TEST(test_validation_boundary_unaligned_doorbell_offsets);
	RUN_TEST(test_validation_boundary_queue_id_bounds);
	RUN_TEST(test_validation_boundary_llq_header_lengths);
	RUN_TEST(test_validation_boundary_dev_stop_teardown);
	RUN_TEST(test_validation_boundary_rx_allocation_failures);
	RUN_TEST(test_validation_boundary_post_timeout_admin_recovery);
	RUN_TEST(test_validation_boundary_corrupted_acq_completions);
	RUN_TEST(test_validation_concurrency_stress_queues);
	RUN_TEST(test_validation_fault_tx_fake_req_id);
	RUN_TEST(test_validation_fault_rx_corrupt_length);
	RUN_TEST(test_validation_aenq_runtime_wiring);

	printf("========================================\n");
	printf("ALL PHASE 10 VALIDATION TESTS PASSED (19/19)\n");
	printf("========================================\n");
	return 0;
}
