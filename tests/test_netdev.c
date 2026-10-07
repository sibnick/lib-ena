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
#include "mock_pci.h"
#include "test_framework.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mock_ena_hw g_hw;
static struct ena_adapter g_adapter;

/* Netbuf tracking for tests that allocate through callbacks */
#define MAX_TRACKED_NETBUFS 16
static struct uk_netbuf *g_tracked_nb[MAX_TRACKED_NETBUFS];
static unsigned int g_tracked_nb_count = 0;

static void test_netdev_setup(void)
{
	mock_ena_hw_init(&g_hw);
	mock_pci_clear_faults(&g_hw);
	test_reset_alloc_tracking();
	g_tracked_nb_count = 0;
}

static void test_netdev_teardown(void)
{
	mock_pci_clear_faults(&g_hw);
}

static void *mock_rx_alloc_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0x8000000;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x1000;
	*len_out = 2048;

	nb->phys_addr = *phys_out;
	nb->buflen = *len_out;
	return nb;
}

static void track_netbuf(struct uk_netbuf *nb)
{
	if (g_tracked_nb_count < MAX_TRACKED_NETBUFS)
		g_tracked_nb[g_tracked_nb_count++] = nb;
}

static void untrack_and_free_netbuf(struct uk_netbuf *nb)
{
	for (unsigned int i = 0; i < g_tracked_nb_count; i++) {
		if (g_tracked_nb[i] == nb) {
			g_tracked_nb[i] = NULL;
			break;
		}
	}
	if (nb) {
		if (nb->data)
			test_free(nb->data);
		test_free(nb);
	}
}

static void free_remaining_tracked_netbufs(void)
{
	for (unsigned int i = 0; i < g_tracked_nb_count; i++)
		if (g_tracked_nb[i])
			untrack_and_free_netbuf(g_tracked_nb[i]);
	g_tracked_nb_count = 0;
}

/* RX alloc callback: small (64-byte) application buffer that takes a
 * bounce slot like the driver helper does. Offers the full 2048-byte
 * slot, so a hostile completion can exceed the application buffer. */
static void *mock_rx_undersized_alloc_cb(void *arg, uint64_t *phys_out,
					 uint32_t *len_out)
{
	struct uk_netdev_rx_queue *rxq = (struct uk_netdev_rx_queue *)arg;
	struct uk_netbuf *nb;
	uint16_t slot;

	nb = test_calloc(1, sizeof(*nb));
	if (!nb)
		return NULL;

	nb->data = test_calloc(1, 64);
	if (!nb->data) {
		test_free(nb);
		return NULL;
	}
	nb->buflen = 64;

	if (!rxq || !rxq->bounce_free_ids || rxq->bounce_free_count == 0) {
		test_free(nb->data);
		test_free(nb);
		return NULL;
	}

	slot = rxq->bounce_free_ids[rxq->bounce_free_head];
	rxq->bounce_free_head =
	    (uint16_t)((rxq->bounce_free_head + 1) & (rxq->nb_desc - 1));
	rxq->bounce_free_count--;
	rxq->pending_slot = (int16_t)slot;

	*phys_out = rxq->bounce_phys + ((uint64_t)slot * ENA_RX_BUF_SIZE);
	*len_out = 2048;

	track_netbuf(nb);
	return nb;
}

static void *mock_rx_standard_alloc_cb(void *arg, uint64_t *phys_out,
				       uint32_t *len_out)
{
	struct uk_netdev_rx_queue *rxq = (struct uk_netdev_rx_queue *)arg;
	struct uk_netbuf *nb;
	uint16_t slot;

	nb = test_calloc(1, sizeof(*nb));
	if (!nb)
		return NULL;

	nb->data = test_calloc(1, 2048);
	if (!nb->data) {
		test_free(nb);
		return NULL;
	}
	nb->buflen = 2048;

	if (!rxq || !rxq->bounce_free_ids || rxq->bounce_free_count == 0) {
		test_free(nb->data);
		test_free(nb);
		return NULL;
	}

	slot = rxq->bounce_free_ids[rxq->bounce_free_head];
	rxq->bounce_free_head =
	    (uint16_t)((rxq->bounce_free_head + 1) & (rxq->nb_desc - 1));
	rxq->bounce_free_count--;
	rxq->pending_slot = (int16_t)slot;

	*phys_out = rxq->bounce_phys + ((uint64_t)slot * ENA_RX_BUF_SIZE);
	*len_out = 2048;

	track_netbuf(nb);
	return nb;
}

static int setup_test_adapter(struct mock_ena_hw *hw,
			      struct ena_adapter *adapter)
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

	adapter->rx_rings =
	    test_calloc(adapter->max_rx_queues, sizeof(struct ena_ring *));
	adapter->tx_rings =
	    test_calloc(adapter->max_tx_queues, sizeof(struct ena_ring *));
	return 0;
}

static void teardown_test_adapter(struct ena_adapter *adapter)
{
	if (adapter->rx_rings) {
		for (uint16_t i = 0; i < adapter->max_rx_queues; i++)
			if (adapter->rx_rings[i])
				ena_ring_free(adapter->rx_rings[i]);
		test_free(adapter->rx_rings);
		adapter->rx_rings = NULL;
	}

	if (adapter->tx_rings) {
		for (uint16_t i = 0; i < adapter->max_tx_queues; i++)
			if (adapter->tx_rings[i])
				ena_ring_free(adapter->tx_rings[i]);
		test_free(adapter->tx_rings);
		adapter->tx_rings = NULL;
	}

	ena_admin_fini(adapter);
}

static void test_netdev_alloc_and_info_get(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_info info;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);
	assert(netdev->state == UK_NETDEV_UNCONFIGURED);

	assert(netdev->ops->info_get(netdev, &info) == 0);
	assert(info.mtu == 1500);
	assert(info.max_rx_queues == g_adapter.max_rx_queues);
	assert(info.max_tx_queues == g_adapter.max_tx_queues);
	assert(info.features & UK_NETDEV_F_PARTIAL_CSUM);
	assert(info.features & UK_NETDEV_F_LRO);
	assert(info.features & UK_NETDEV_F_TSO4);
	assert(info.hwaddr[0] == 0x52 && info.hwaddr[1] == 0x54);

	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_configure_and_lifecycle(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->state == UK_NETDEV_CONFIGURED);
	assert(netdev->nb_rx_queues == 2);
	assert(netdev->nb_tx_queues == 2);

	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->rxq_configure(netdev, 1, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 1, 8, NULL) == 0);

	/* Start device */
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);

	/* Stop device */
	assert(netdev->ops->dev_stop(netdev) == 0);
	assert(netdev->state == UK_NETDEV_STOPPED);

	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_multi_queue_msix_mapping(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	void *rx_arr;
	void *tx_arr;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	/* Four MSI-X vectors: vector 0 for the admin/AENQ queue,
	 * vectors 1..3 for the IO queues. */
	ena_plat_set_mock_msix_vectors(4);
	assert(ena_intr_setup(&g_adapter, NULL) == 0);
	assert(g_adapter.num_irq_vectors == 4);
	assert(g_adapter.irq_vectors[0].is_admin == true);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	rx_arr = g_adapter.rx_rings;
	tx_arr = g_adapter.tx_rings;

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->rxq_configure(netdev, 1, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 1, 8, NULL) == 0);

	/* Start device */
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);

	/* Every configured queue is live after start */
	assert(g_adapter.rx_rings[0] != NULL);
	assert(g_adapter.rx_rings[1] != NULL);
	assert(g_adapter.tx_rings[0] != NULL);
	assert(g_adapter.tx_rings[1] != NULL);

	/* IO queue q is bound to MSI-X vector q+1. Vector 0 stays
	 * reserved for the admin/AENQ queue. */
	assert(g_hw.cq_msix[g_adapter.tx_rings[0]->cq_idx] == 1);
	assert(g_hw.cq_msix[g_adapter.tx_rings[1]->cq_idx] == 2);
	assert(g_hw.cq_msix[g_adapter.rx_rings[0]->cq_idx] == 1);
	assert(g_hw.cq_msix[g_adapter.rx_rings[1]->cq_idx] == 2);

	/* Stop device */
	assert(netdev->ops->dev_stop(netdev) == 0);
	assert(netdev->state == UK_NETDEV_STOPPED);

	ena_netdev_free(netdev);

	/* Restore polling mode for the remaining tests */
	ena_plat_set_mock_msix_vectors(0);

	if (rx_arr)
		test_track_free(rx_arr);
	if (tx_arr)
		test_track_free(tx_arr);
}

static void test_netdev_txq_xmit(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = test_calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* Transmit packet */
	tx_buf->phys_addr = 0x50001000;
	tx_buf->len = 256;

	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);
	assert(g_adapter.tx_rings[0]->sq_tail == 1);
	assert(g_adapter.tx_rings[0]->tx_packets == 1);
	assert(mock_ena_hw_get_reg32(&g_hw,
				     g_adapter.tx_rings[0]->sq_db_offset) == 1);

	/* Mock device completes packet */
	mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 1);

	/* Transmit second packet (triggers poll) */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);
	assert(g_adapter.tx_rings[0]->sq_head == 1);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_tx_bounce_backpressure(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
	char pkt_data[64] = {0};
	struct uk_netdev_tx_queue *txq;
	struct ena_ring *ring;
	int i;
	int ret;
	int busy = 0;

	assert(nb != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	txq = &netdev->tx_queues[0];
	ring = g_adapter.tx_rings[0];

	/* The bounce buffer is pre-allocated in txq_configure, so the fast
	 * path does not allocate in the data path. */
	assert(txq->bounce_buf != NULL);

	/* A low-memory packet (physical address below the limit) uses the
	 * bounce buffer. Its completion is never delivered by the mock. */
	nb->data = pkt_data;
	nb->len = sizeof(pkt_data);
	nb->phys_addr = 0x1000;

	assert(netdev->ops->txq_xmit(netdev, 0, nb) == 0);
	assert(txq->bounce_in_use == true);
	assert(txq->bounce_wait_polls == 0);

	/* Fill remaining bounce ring pool slots */
	for (i = 1; i < 8; i++)
		assert(netdev->ops->txq_xmit(netdev, 0, nb) == 0);
	assert(txq->bounce_free_count == 0);

	/* The pool is full and the mock never completes these requests.
	 * The driver must NOT force-release a request: that frees a req_id
	 * whose completion is still outstanding, the ID is reused, and the
	 * late completion then frees the wrong netbuf (a double free that
	 * corrupts the heap). It must apply backpressure instead: every
	 * further low-memory transmit returns -EBUSY. [Ticket a9c6945c21] */
	for (i = 0; i < ENA_TX_BOUNCE_STALL_LIMIT + 1; i++) {
		ret = netdev->ops->txq_xmit(netdev, 0, nb);
		assert(ret == -EBUSY);
		busy++;
	}
	assert(busy == ENA_TX_BOUNCE_STALL_LIMIT + 1);

	/* No request was force-released: the pool stays full and the last
	 * submitted request is still in flight. */
	assert(txq->bounce_free_count == 0);
	assert(ring->req_in_flight[txq->bounce_req_id] == 1);

	/* Real completions arrive in order and free the request IDs. The
	 * next transmit reclaims the bounce slots internally. */
	mock_ena_hw_emulate_tx(&g_hw, ring, 8);
	ena_tx_poll_completions(ring, 8, NULL);

	nb->phys_addr = 0x50001000;
	assert(netdev->ops->txq_xmit(netdev, 0, nb) == 0);
	assert(txq->bounce_free_count == 8);
	assert(txq->bounce_in_use == false);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(nb);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_rxq_recv(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* Populate RX ring */
	assert(ena_rx_refill(g_adapter.rx_rings[0], 4, mock_rx_alloc_cb, NULL,
			     &refilled) == 4);

	/* Receive before arrival returns 0 */
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);

	/* Mock incoming packet */
	mock_ena_hw_emulate_rx(&g_hw, g_adapter.rx_rings[0], 1, 512, 0x11223344,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 512);
	assert(rx_buf->hash == 0x11223344);
	assert(rx_buf->l4_csum_checked == true);
	assert(rx_buf->l3_csum_err == false);

	test_free(rx_buf);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < g_adapter.rx_rings[0]->sq_depth; i++) {
		if (g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			test_free(
			    g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			g_adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_rx_undersized_netbuf(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netdev_rx_queue *rxq;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	uint8_t *slot_virt;
	uint8_t expect[64];
	uint16_t next_slot;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	rxq = &netdev->rx_queues[0];

	/* Populate 4 buffers, each taking one bounce slot */
	assert(ena_rx_refill(rx_ring, 4, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 4);
	assert(refilled == 4);
	assert(rxq->bounce_free_count == 4);

	/* Hostile device: 100-byte completion into a 64-byte application
	 * buffer. The driver must drop the packet, not overflow the buffer. */
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys;
	memset(slot_virt, 0xCC, 100);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 100, 0, 0);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rx_buf == NULL);
	assert(rxq->bounce_free_count == 5);
	/* The ring counted the completion; the netdev layer dropped the packet
	 */
	assert(rx_ring->rx_packets == 1);

	/* The dropped netbuf is orphaned: the test frees it */
	untrack_and_free_netbuf(g_tracked_nb[0]);

	/* A packet that fits the 64-byte buffer is delivered with its
	 * payload copied from the bounce slot */
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys +
		    (size_t)1 * ENA_RX_BUF_SIZE;
	memset(slot_virt, 0x77, 64);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 64, 0x12345678,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 64);
	assert(rx_buf->hash == 0x12345678);
	memset(expect, 0x77, sizeof(expect));
	assert(memcmp(rx_buf->data, expect, 64) == 0);
	assert(rxq->bounce_free_count == 6);
	untrack_and_free_netbuf(g_tracked_nb[1]);

	/* The ring still refills and delivers after the drop */
	assert(ena_rx_refill(rx_ring, 1, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 1);
	assert(rxq->bounce_free_count == 5);
	/* Pre-fill the bounce slot the next completion will land in */
	next_slot =
	    (uint16_t)
		rxq->bounce_map[rx_ring->sq_head & (rx_ring->sq_depth - 1)];
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys +
		    (size_t)next_slot * ENA_RX_BUF_SIZE;
	memset(slot_virt, 0x55, 64);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 64, 0, 0);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf->len == 64);
	assert(rxq->bounce_free_count == 6);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free_remaining_tracked_netbufs();
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_rx_bad_completion_bounce_pool(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netdev_rx_queue *rxq;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	uint8_t *slot_virt;
	uint8_t expect[64];
	unsigned int i;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	rxq = &netdev->rx_queues[0];

	assert(ena_rx_refill(rx_ring, 4, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 4);
	assert(rxq->bounce_free_count == 4);
	assert(rx_ring->free_req_count == 4);

	/* A faulty or hostile device repeats over-length completions.
	 * Every drop must return its bounce slot to the free pool. */
	for (i = 0; i < 4; i++) {
		mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_CORRUPT_LENGTH,
				      0xFFFF);
		mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 512, 0, 0);
		mock_pci_clear_faults(&g_hw);

		rx_buf = NULL;
		assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
		assert(rx_buf == NULL);
		assert(rx_ring->rx_packets == 0);
		assert(rxq->bounce_free_count == 4 + (i + 1));
		assert(rx_ring->free_req_count == 4 + (i + 1));

		untrack_and_free_netbuf(g_tracked_nb[i]);
	}

	/* The pool is intact: the ring refills and a good packet is
	 * still delivered (receive did not deadlock) */
	assert(ena_rx_refill(rx_ring, 1, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 1);
	assert(rxq->bounce_free_count == 7);

	uint16_t refill_slot =
	    (uint16_t)
		rxq->bounce_map[rx_ring->sq_head & (rx_ring->sq_depth - 1)];
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys +
		    (size_t)refill_slot * ENA_RX_BUF_SIZE;
	memset(slot_virt, 0x44, 64);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 64, 0x99887766, 0);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 64);
	assert(rx_buf->hash == 0x99887766);
	assert(rxq->bounce_free_count == 8);

	memset(expect, 0x44, sizeof(expect));
	assert(memcmp(rx_buf->data, expect, 64) == 0);
	untrack_and_free_netbuf(g_tracked_nb[4]);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free_remaining_tracked_netbufs();
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_invalid_ops(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = test_calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;

	/* Operations when unconfigured or stopped */
	tx_buf->len = 64;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == -EAGAIN);
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == -EAGAIN);

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);

	/* Invalid queue index */
	assert(netdev->ops->rxq_configure(netdev, 5, 8, NULL) == -EINVAL);
	assert(netdev->ops->txq_configure(netdev, 5, 8, NULL) == -EINVAL);

	assert(netdev->ops->dev_start(netdev) == 0);

	/* Reconfigure while running */
	assert(netdev->ops->configure(netdev, &conf) == -EBUSY);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == -EBUSY);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == -EBUSY);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_buf);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static uint16_t mock_low_mem_rx_alloc(void *arg, struct uk_netbuf *pkts[],
				      uint16_t count)
{
	(void)arg;
	for (uint16_t i = 0; i < count; i++) {
		pkts[i] = test_calloc(1, sizeof(struct uk_netbuf));
		if (!pkts[i])
			return i;
		pkts[i]->data = test_calloc(1, 2048);
		pkts[i]->phys_addr = 0x500; /* Below 1MB */
		pkts[i]->buflen = 2048;
	}
	return count;
}

static void test_netdev_bounce_buffers(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct uk_netbuf *tx_buf1 = test_calloc(1, sizeof(*tx_buf1));
	struct uk_netbuf *tx_buf2 = test_calloc(1, sizeof(*tx_buf2));
	uint8_t payload1[64];
	uint8_t payload2[64];

	memset(payload1, 0xAA, sizeof(payload1));
	memset(payload2, 0xBB, sizeof(payload2));

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	rx_conf.alloc_rxpkts = mock_low_mem_rx_alloc;
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);

	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* TX bounce buffer test */
	tx_buf1->data = payload1;
	tx_buf1->phys_addr = 0x500; /* Low memory address */
	tx_buf1->len = sizeof(payload1);

	tx_buf2->data = payload2;
	tx_buf2->phys_addr = 0x600; /* Low memory address */
	tx_buf2->len = sizeof(payload2);

	int k;

	/* First packet transmits using bounce buffer */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf1) == 0);
	assert(netdev->tx_queues[0].bounce_in_use == true);

	/* Second low-memory packet also transmits using next slot in bounce
	 * ring pool */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == 0);
	assert(netdev->tx_queues[0].bounce_in_use == true);

	/* Fill remaining slots until bounce buffer ring pool is exhausted */
	for (k = 2; k < 8; k++)
		assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == 0);

	/* Next low-memory packet fails with -EBUSY when bounce pool is
	 * exhausted */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == -EBUSY);

	/* Complete transmissions */
	mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 8);
	ena_tx_poll_completions(g_adapter.tx_rings[0], 8, NULL);

	/* After completion, subsequent packet transmits successfully */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == 0);
	assert(netdev->tx_queues[0].bounce_in_use == true);

	mock_ena_hw_emulate_tx(&g_hw, g_adapter.tx_rings[0], 1);
	ena_tx_poll_completions(g_adapter.tx_rings[0], 1, NULL);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(tx_buf1);
	test_free(tx_buf2);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* TX bounce slots must be reclaimed at completion time through the ring
 * completion callback, not by a scan on the next send. A bare
 * ena_tx_poll_completions call must free the slots. An idle ring with no
 * request in flight must skip the completion peek. [Ticket 292e049bf4] */
static void test_netdev_tx_reclaim_at_completion(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_tx_queue *txq;
	struct ena_ring *ring;
	struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
	uint8_t payload[64];
	unsigned int cleaned = 0;
	int i;

	assert(nb != NULL);
	memset(payload, 0x5A, sizeof(payload));

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	txq = &netdev->tx_queues[0];
	ring = g_adapter.tx_rings[0];

	/* Four low-memory sends take four bounce slots. */
	nb->data = payload;
	nb->len = sizeof(payload);
	nb->phys_addr = 0x1000;
	for (i = 0; i < 4; i++)
		assert(netdev->ops->txq_xmit(netdev, 0, nb) == 0);
	assert(txq->bounce_free_count == 4);

	/* Completions arrive. A bare completion poll must reclaim each slot
	 * through the callback. The old code held the slots until the next
	 * send scanned the whole map. */
	mock_ena_hw_emulate_tx(&g_hw, ring, 4);
	assert(ena_tx_poll_completions(ring, 8, &cleaned) == 4);
	assert(cleaned == 4);
	assert(txq->bounce_free_count == 8);
	assert(txq->bounce_in_use == false);

	/* The ring is idle: no request is in flight. A further poll must
	 * skip the lock and the CQ read and report no work. */
	cleaned = 123;
	assert(ena_tx_poll_completions(ring, 8, &cleaned) == 0);
	assert(cleaned == 0);

	assert(netdev->ops->dev_stop(netdev) == 0);
	test_free(nb);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_start_rollback(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->rxq_configure(netdev, 1, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 1, 8, NULL) == 0);

	/* Inject fault during hardware ring creation */
	mock_pci_inject_fault(&g_hw, MOCK_PCI_FAULT_BAD_DB_OFFSET, 0x9000);
	int ret = netdev->ops->dev_start(netdev);
	assert(ret != 0);
	assert(netdev->state != UK_NETDEV_RUNNING);

	/* Clear fault and verify start succeeds */
	mock_pci_clear_faults(&g_hw);
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);

	assert(netdev->ops->dev_stop(netdev) == 0);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_free_running_teardown(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	void *rx_arr;
	void *tx_arr;
	uint32_t sq_created;
	uint32_t cq_created;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	rx_arr = g_adapter.rx_rings;
	tx_arr = g_adapter.tx_rings;

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 2;
	conf.nb_tx_queues = 2;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->rxq_configure(netdev, 1, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 1, 8, NULL) == 0);

	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);

	sq_created = g_hw.sq_created_count;
	cq_created = g_hw.cq_created_count;
	assert(sq_created > 0);
	assert(cq_created > 0);

	/*
	 * Freeing a running device must complete the full teardown the
	 * netdev API cannot express: stop the datapath (destroy every SQ
	 * and CQ), release the bounce buffers, the rings, the admin
	 * queues, and the host info buffer, then free the netdev struct.
	 */
	ena_netdev_free(netdev);

	/* The driver frees the ring arrays with plain free(); balance
	 * the test allocator tracker. */
	if (rx_arr)
		test_track_free(rx_arr);
	if (tx_arr)
		test_track_free(tx_arr);

	/* Every queue created at start is destroyed at stop. */
	assert(g_hw.sq_destroyed_count == sq_created);
	assert(g_hw.cq_destroyed_count == cq_created);

	/* The adapter is stopped and all driver-owned memory is released. */
	assert(g_adapter.state == ENA_STATE_STOPPED);
	assert(g_adapter.aq_base == NULL);
	assert(g_adapter.acq_base == NULL);
	assert(g_adapter.aenq_base == NULL);
	assert(g_adapter.host_info_base == NULL);
	assert(g_adapter.host_info_phys == 0);
	assert(g_adapter.rx_rings == NULL);
	assert(g_adapter.tx_rings == NULL);

	/* A second fini on the already-released adapter is a no-op. */
	ena_admin_fini(&g_adapter);
	assert(g_adapter.state == ENA_STATE_STOPPED);
}

static void test_netdev_free_not_running(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	void *rx_arr;
	void *tx_arr;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);

	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	rx_arr = g_adapter.rx_rings;
	tx_arr = g_adapter.tx_rings;

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);

	/*
	 * Freeing a configured but never-started device must release the
	 * admin queues and the rings without issuing hardware destroys.
	 */
	ena_netdev_free(netdev);

	if (rx_arr)
		test_track_free(rx_arr);
	if (tx_arr)
		test_track_free(tx_arr);

	assert(g_hw.sq_destroyed_count == 0);
	assert(g_hw.cq_destroyed_count == 0);

	assert(g_adapter.state == ENA_STATE_STOPPED);
	assert(g_adapter.aq_base == NULL);
	assert(g_adapter.acq_base == NULL);
	assert(g_adapter.aenq_base == NULL);
	assert(g_adapter.host_info_base == NULL);
	assert(g_adapter.rx_rings == NULL);
	assert(g_adapter.tx_rings == NULL);
}

static void test_netdev_rx_csum_and_lro_chaining(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netdev_rx_queue *rxq;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	conf.lro = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	rxq = &netdev->rx_queues[0];

	/* Test 1: Hardware RX checksum ok sets DATA_VALID */
	assert(ena_rx_refill(rx_ring, 1, mock_rx_standard_alloc_cb, rxq,
			     &refilled) == 1);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 64, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->flags & UK_NETBUF_F_DATA_VALID);

	/* Test 2: Multi-descriptor LRO chain reassembly */
	assert(ena_rx_refill(rx_ring, 2, mock_rx_standard_alloc_cb, rxq,
			     &refilled) == 2);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 1400, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 600, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 1400);
	assert(rx_buf->next != NULL);
	assert(rx_buf->next->len == 600);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free_remaining_tracked_netbufs();
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static void test_netdev_tx_csum_offload(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct uk_netbuf *tx_buf;
	struct ena_ring *tx_ring;
	const struct ena_eth_io_tx_desc *slot_desc;
	uint8_t dummy_tcp_pkt[64];

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	tx_ring = g_adapter.tx_rings[0];

	/* Build IPv4 TCP frame template */
	memset(dummy_tcp_pkt, 0, sizeof(dummy_tcp_pkt));
	dummy_tcp_pkt[12] = 0x08;
	dummy_tcp_pkt[13] = 0x00;  /* IPv4 */
	dummy_tcp_pkt[14 + 9] = 6; /* TCP */

	tx_buf = test_calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);
	tx_buf->data = dummy_tcp_pkt;
	tx_buf->len = sizeof(dummy_tcp_pkt);
	tx_buf->phys_addr = 0x50001000;

	/* Transmit with UK_NETBUF_F_PARTIAL_CSUM enabled */
	tx_buf->flags = UK_NETBUF_F_PARTIAL_CSUM;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	slot_desc = (const struct ena_eth_io_tx_desc *)tx_ring->sq_virt;
	assert(slot_desc[0].meta_ctrl & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK);
	assert(slot_desc[0].meta_ctrl & ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK);

	/* Clean completions */
	mock_ena_hw_emulate_tx(&g_hw, tx_ring, 1);
	ena_tx_poll_completions(tx_ring, 8, NULL);

	/* Transmit with UK_NETBUF_F_PARTIAL_CSUM disabled */
	tx_buf->flags = 0;
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);

	slot_desc = (const struct ena_eth_io_tx_desc *)tx_ring->sq_virt;
	assert((slot_desc[1].meta_ctrl & ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK) ==
	       0);
	assert((slot_desc[1].meta_ctrl & ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK) ==
	       0);

	mock_ena_hw_emulate_tx(&g_hw, tx_ring, 1);
	ena_tx_poll_completions(tx_ring, 8, NULL);

	test_free(tx_buf);
	assert(netdev->ops->dev_stop(netdev) == 0);
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

static bool g_burst_alloc_fail = false;

static uint16_t mock_rx_burst_alloc_pkts(void *argp, struct uk_netbuf *pkts[],
					 uint16_t count)
{
	(void)argp;
	if (g_burst_alloc_fail)
		return 0;

	for (uint16_t i = 0; i < count; i++) {
		struct uk_netbuf *nb = test_calloc(1, sizeof(*nb));
		nb->buflen = 2048;
		nb->data = test_calloc(1, 2048);
		nb->phys_addr = (uint64_t)(uintptr_t)nb->data;
		pkts[i] = nb;
		track_netbuf(nb);
	}
	return count;
}

static void test_netdev_rx_burst_refill_and_state_dump(void)
{
	printf(
	    "[TEST] Running test_netdev_rx_burst_refill_and_state_dump...\n");

	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	int ret;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	rx_conf.alloc_rxpkts = mock_rx_burst_alloc_pkts;
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];

	/* Initially populate 4 descriptors */
	g_burst_alloc_fail = false;
	assert(ena_rx_refill(rx_ring, 4, mock_rx_alloc_cb, NULL, &refilled) ==
	       4);
	assert(rx_ring->free_req_count == 4);

	/* Simulate memory exhaustion during burst */
	g_burst_alloc_fail = true;
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 4, 128, 0, 0);

	/* Consume all 4 packets. Because allocator fails, refill cannot succeed
	 */
	for (int i = 0; i < 4; i++) {
		rx_buf = NULL;
		ret = netdev->ops->rxq_recv(netdev, 0, &rx_buf);
		assert((ret & UK_NETDEV_STATUS_SUCCESS) != 0);
		assert(rx_buf != NULL);
		untrack_and_free_netbuf(rx_buf);
	}

	/* Ring is now depleted: all 8 slots are free, SQ has 0 descriptors in
	 * hardware */
	assert(rx_ring->free_req_count == 8);
	assert(rx_ring->rx_refill_err > 0);

	/* Diagnostic dump runs cleanly during stall state */
	ena_ring_dump_state(rx_ring);
	ena_netdev_dump_queue(netdev, 0);

	/* Idle poll while allocator still fails: returns 0, remains depleted */
	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rx_ring->free_req_count == 8);

	/* Memory frees up: allocator succeeds again */
	g_burst_alloc_fail = false;

	/* Idle poll replenishes all depleted descriptors */
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rx_ring->free_req_count == 0);

	/* Hardware can now receive new incoming packets */
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 128, 0, 0);
	rx_buf = NULL;
	ret = netdev->ops->rxq_recv(netdev, 0, &rx_buf);
	assert((ret & UK_NETDEV_STATUS_SUCCESS) != 0);
	assert(rx_buf != NULL);
	untrack_and_free_netbuf(rx_buf);

	assert(netdev->ops->dev_stop(netdev) == 0);
	teardown_test_adapter(&g_adapter);
	free_remaining_tracked_netbufs();
	ena_netdev_free(netdev);

	printf("[PASS] test_netdev_rx_burst_refill_and_state_dump passed\n");
}

static void test_netdev_rx_more_flag(void)
{
	printf("[TEST] Running test_netdev_rx_more_flag...\n");

	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	int ret;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];

	/* Populate 4 buffers */
	assert(ena_rx_refill(rx_ring, 4, mock_rx_alloc_cb, NULL, &refilled) ==
	       4);

	/* Emulate arrival of 2 packets in hardware completion queue */
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 2, 256, 0, 0);

	/* First packet: more flag must be set */
	rx_buf = NULL;
	ret = netdev->ops->rxq_recv(netdev, 0, &rx_buf);
	assert((ret & UK_NETDEV_STATUS_SUCCESS) != 0);
	assert((ret & UK_NETDEV_STATUS_MORE) != 0);
	assert(rx_buf != NULL);
	test_free(rx_buf);

	/* Second packet: queue now empty so more flag must be cleared */
	rx_buf = NULL;
	ret = netdev->ops->rxq_recv(netdev, 0, &rx_buf);
	assert((ret & UK_NETDEV_STATUS_SUCCESS) != 0);
	assert((ret & UK_NETDEV_STATUS_MORE) == 0);
	assert(rx_buf != NULL);
	test_free(rx_buf);

	/* Third poll: queue is empty, returns 0 */
	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (uint16_t i = 0; i < rx_ring->sq_depth; i++) {
		if (rx_ring->buffers.rx_bufs[i].netbuf) {
			test_free(rx_ring->buffers.rx_bufs[i].netbuf);
			rx_ring->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	teardown_test_adapter(&g_adapter);
	free_remaining_tracked_netbufs();
	ena_netdev_free(netdev);

	printf("[PASS] test_netdev_rx_more_flag passed\n");
}

/*
 * A dropped fragment ends the frame. The driver must free the fragments
 * it already chained and clear the chain pointers, or the next packet
 * carries a half-received frame. [Ticket 6fd56c8d29]
 */
static void test_netdev_rx_chain_drop_clears_chain(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netdev_rx_queue *rxq;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	uint8_t *slot_virt;
	uint16_t next_slot;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	rxq = &netdev->rx_queues[0];

	assert(ena_rx_refill(rx_ring, 2, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 2);

	/* First fragment of a two-descriptor frame: the driver chains
	 * it and waits for the rest. */
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys;
	memset(slot_virt, 0xAA, 40);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 40, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rx_buf == NULL);
	assert(rxq->chain_head != NULL);
	assert(rxq->chain_tail == rxq->chain_head);

	/* The second fragment does not fit the application buffer. The
	 * driver drops it and must clear the chain with the first
	 * fragment. */
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys +
		    (size_t)1 * ENA_RX_BUF_SIZE;
	memset(slot_virt, 0xBB, 100);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 100, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rx_buf == NULL);
	assert(rxq->chain_head == NULL);
	assert(rxq->chain_tail == NULL);
	assert(rx_ring->rx_dropped == 1);

	/* The next packet is delivered on its own. It must not carry
	 * the fragments of the dropped frame. */
	assert(ena_rx_refill(rx_ring, 1, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 1);
	next_slot =
	    (uint16_t)
		rxq->bounce_map[rx_ring->sq_head & (rx_ring->sq_depth - 1)];
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys +
		    (size_t)next_slot * ENA_RX_BUF_SIZE;
	memset(slot_virt, 0xCC, 40);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 40, 0, 0);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 40);
	assert(rx_buf->next == NULL);
	untrack_and_free_netbuf(rx_buf);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free_remaining_tracked_netbufs();
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

/* Stop must not leave a partial frame behind in a queue. */
static void test_netdev_rx_stop_clears_chain(void)
{
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct ena_ring *rx_ring;
	struct uk_netdev_rx_queue *rxq;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;
	uint8_t *slot_virt;

	assert(setup_test_adapter(&g_hw, &g_adapter) == 0);
	netdev = ena_netdev_alloc(&g_adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	assert(netdev->ops->configure(netdev, &conf) == 0);

	memset(&rx_conf, 0, sizeof(rx_conf));
	memset(&tx_conf, 0, sizeof(tx_conf));
	assert(netdev->ops->rxq_configure(netdev, 0, 8, &rx_conf) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, &tx_conf) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	rx_ring = g_adapter.rx_rings[0];
	rxq = &netdev->rx_queues[0];

	assert(ena_rx_refill(rx_ring, 1, mock_rx_undersized_alloc_cb, rxq,
			     &refilled) == 1);
	slot_virt = (uint8_t *)(uintptr_t)rxq->bounce_phys;
	memset(slot_virt, 0xAA, 40);
	mock_ena_hw_emulate_rx(&g_hw, rx_ring, 1, 40, 0,
			       ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK);

	rx_buf = NULL;
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);
	assert(rxq->chain_head != NULL);

	assert(netdev->ops->dev_stop(netdev) == 0);
	assert(rxq->chain_head == NULL);
	assert(rxq->chain_tail == NULL);

	free_remaining_tracked_netbufs();
	teardown_test_adapter(&g_adapter);
	ena_netdev_free(netdev);
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 7 Test Suite \n");
	printf("========================================\n");

	test_register_setup(test_netdev_setup);
	test_register_teardown(test_netdev_teardown);

	RUN_TEST(test_netdev_alloc_and_info_get);
	RUN_TEST(test_netdev_configure_and_lifecycle);
	RUN_TEST(test_netdev_multi_queue_msix_mapping);
	RUN_TEST(test_netdev_txq_xmit);
	RUN_TEST(test_netdev_tx_bounce_backpressure);
	RUN_TEST(test_netdev_rxq_recv);
	RUN_TEST(test_netdev_rx_csum_and_lro_chaining);
	RUN_TEST(test_netdev_tx_csum_offload);
	RUN_TEST(test_netdev_rx_undersized_netbuf);
	RUN_TEST(test_netdev_rx_bad_completion_bounce_pool);
	RUN_TEST(test_netdev_rx_chain_drop_clears_chain);
	RUN_TEST(test_netdev_rx_stop_clears_chain);
	RUN_TEST(test_netdev_invalid_ops);
	RUN_TEST(test_netdev_bounce_buffers);
	RUN_TEST(test_netdev_tx_reclaim_at_completion);
	RUN_TEST(test_netdev_start_rollback);
	RUN_TEST(test_netdev_free_running_teardown);
	RUN_TEST(test_netdev_free_not_running);
	RUN_TEST(test_netdev_rx_burst_refill_and_state_dump);
	RUN_TEST(test_netdev_rx_more_flag);

	printf("========================================\n");
	printf("ALL PHASE 7 NETDEV TESTS PASSED (20/20) \n");
	printf("========================================\n");
	return 0;
}
