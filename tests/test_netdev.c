/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_netdev.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *mock_rx_alloc_cb(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	static uint64_t next_phys = 0x8000000;
	struct uk_netbuf *nb = calloc(1, sizeof(*nb));
	(void)arg;

	*phys_out = next_phys;
	next_phys += 0x1000;
	*len_out = 2048;

	nb->phys_addr = *phys_out;
	nb->buflen = *len_out;
	return nb;
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

static void test_netdev_alloc_and_info_get(void)
{
	printf("[TEST] Running test_netdev_alloc_and_info_get...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_info info;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);
	assert(netdev->state == UK_NETDEV_UNCONFIGURED);

	assert(netdev->ops->info_get(netdev, &info) == 0);
	assert(info.mtu == 1500);
	assert(info.max_rx_queues == adapter.max_rx_queues);
	assert(info.max_tx_queues == adapter.max_tx_queues);
	assert(info.features & UK_NETDEV_F_RX_CSUM);
	assert(info.features & UK_NETDEV_F_TX_CSUM);
	assert(info.hwaddr[0] == 0x52 && info.hwaddr[1] == 0x54);

	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_alloc_and_info_get passed\n");
}

static void test_netdev_configure_and_lifecycle(void)
{
	printf("[TEST] Running test_netdev_configure_and_lifecycle...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	netdev = ena_netdev_alloc(&adapter);
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

	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_configure_and_lifecycle passed\n");
}

static void test_netdev_txq_xmit(void)
{
	printf("[TEST] Running test_netdev_txq_xmit...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = calloc(1, sizeof(*tx_buf));
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&hw, &adapter) == 0);

	netdev = ena_netdev_alloc(&adapter);
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
	assert(adapter.tx_rings[0]->sq_tail == 1);
	assert(adapter.tx_rings[0]->tx_packets == 1);
	assert(mock_ena_hw_get_reg32(&hw, adapter.tx_rings[0]->sq_db_offset) == 1);

	/* Mock device completes packet */
	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);

	/* Transmit second packet (triggers poll) */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf) == 0);
	assert(adapter.tx_rings[0]->sq_head == 1);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_txq_xmit passed\n");
}

static void test_netdev_rxq_recv(void)
{
	printf("[TEST] Running test_netdev_rxq_recv...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *rx_buf = NULL;
	unsigned int refilled;

	assert(setup_test_adapter(&hw, &adapter) == 0);

	netdev = ena_netdev_alloc(&adapter);
	assert(netdev != NULL);

	memset(&conf, 0, sizeof(conf));
	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;

	assert(netdev->ops->configure(netdev, &conf) == 0);
	assert(netdev->ops->rxq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->txq_configure(netdev, 0, 8, NULL) == 0);
	assert(netdev->ops->dev_start(netdev) == 0);

	/* Populate RX ring */
	assert(ena_rx_refill(adapter.rx_rings[0], 4, mock_rx_alloc_cb, NULL, &refilled) == 4);

	/* Receive before arrival returns 0 */
	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 0);

	/* Mock incoming packet */
	mock_ena_hw_emulate_rx(&hw, adapter.rx_rings[0], 1, 512, 0x11223344,
			       ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);

	assert(netdev->ops->rxq_recv(netdev, 0, &rx_buf) == 1);
	assert(rx_buf != NULL);
	assert(rx_buf->len == 512);
	assert(rx_buf->hash == 0x11223344);
	assert(rx_buf->l4_csum_checked == true);
	assert(rx_buf->l3_csum_err == false);

	free(rx_buf);

	assert(netdev->ops->dev_stop(netdev) == 0);
	for (int i = 0; i < adapter.rx_rings[0]->sq_depth; i++) {
		if (adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf) {
			free(adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf);
			adapter.rx_rings[0]->buffers.rx_bufs[i].netbuf = NULL;
		}
	}
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_rxq_recv passed\n");
}

static void test_netdev_invalid_ops(void)
{
	printf("[TEST] Running test_netdev_invalid_ops...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netbuf *tx_buf = calloc(1, sizeof(*tx_buf));
	struct uk_netbuf *rx_buf = NULL;
	assert(tx_buf != NULL);

	assert(setup_test_adapter(&hw, &adapter) == 0);

	netdev = ena_netdev_alloc(&adapter);
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

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_invalid_ops passed\n");
}

static uint16_t mock_low_mem_rx_alloc(void *arg, struct uk_netbuf *pkts[], uint16_t count)
{
	(void)arg;
	for (uint16_t i = 0; i < count; i++) {
		pkts[i] = calloc(1, sizeof(struct uk_netbuf));
		if (!pkts[i])
			return i;
		pkts[i]->data = calloc(1, 2048);
		pkts[i]->phys_addr = 0x500; /* Below 1MB */
		pkts[i]->buflen = 2048;
	}
	return count;
}

static void test_netdev_bounce_buffers(void)
{
	printf("[TEST] Running test_netdev_bounce_buffers...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	struct uk_netbuf *tx_buf1 = calloc(1, sizeof(*tx_buf1));
	struct uk_netbuf *tx_buf2 = calloc(1, sizeof(*tx_buf2));
	uint8_t payload1[64];
	uint8_t payload2[64];

	memset(payload1, 0xAA, sizeof(payload1));
	memset(payload2, 0xBB, sizeof(payload2));

	assert(setup_test_adapter(&hw, &adapter) == 0);
	netdev = ena_netdev_alloc(&adapter);
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

	/* First packet transmits using bounce buffer */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf1) == 0);
	assert(netdev->tx_queues[0].bounce_in_use == true);

	/* Second low-memory packet fails with -EBUSY while bounce buffer is in flight */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == -EBUSY);

	/* Complete first transmission */
	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
	ena_tx_poll_completions(adapter.tx_rings[0], 1, NULL);

	/* After completion, second packet transmits successfully */
	assert(netdev->ops->txq_xmit(netdev, 0, tx_buf2) == 0);
	assert(netdev->tx_queues[0].bounce_in_use == true);

	mock_ena_hw_emulate_tx(&hw, adapter.tx_rings[0], 1);
	ena_tx_poll_completions(adapter.tx_rings[0], 1, NULL);

	assert(netdev->ops->dev_stop(netdev) == 0);
	free(tx_buf1);
	free(tx_buf2);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_bounce_buffers passed\n");
}

static void test_netdev_start_rollback(void)
{
	printf("[TEST] Running test_netdev_start_rollback...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	struct uk_netdev *netdev;
	struct uk_netdev_conf conf;

	assert(setup_test_adapter(&hw, &adapter) == 0);
	netdev = ena_netdev_alloc(&adapter);
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
	mock_pci_inject_fault(&hw, MOCK_PCI_FAULT_BAD_DB_OFFSET, 0x9000);
	int ret = netdev->ops->dev_start(netdev);
	assert(ret != 0);
	assert(netdev->state != UK_NETDEV_RUNNING);

	/* Clear fault and verify start succeeds */
	mock_pci_clear_faults(&hw);
	assert(netdev->ops->dev_start(netdev) == 0);
	assert(netdev->state == UK_NETDEV_RUNNING);

	assert(netdev->ops->dev_stop(netdev) == 0);
	ena_netdev_free(netdev);
	teardown_test_adapter(&adapter);

	printf("[PASS] test_netdev_start_rollback passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 7 Test Suite \n");
	printf("========================================\n");

	test_netdev_alloc_and_info_get();
	test_netdev_configure_and_lifecycle();
	test_netdev_txq_xmit();
	test_netdev_rxq_recv();
	test_netdev_invalid_ops();
	test_netdev_bounce_buffers();
	test_netdev_start_rollback();

	printf("========================================\n");
	printf("ALL PHASE 7 NETDEV TESTS PASSED (7/7)   \n");
	printf("========================================\n");
	return 0;
}
