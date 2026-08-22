/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_netdev.h"
#include "ena_datapath.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int ena_netdev_info_get(struct uk_netdev *dev, struct uk_netdev_info *info)
{
	if (!dev || !info || !dev->adapter)
		return -EINVAL;

	info->max_rx_queues = dev->adapter->max_rx_queues;
	info->max_tx_queues = dev->adapter->max_tx_queues;
	info->max_mtu = dev->adapter->max_mtu;
	info->min_mtu = 68;
	info->mtu = dev->adapter->mtu;
	memcpy(info->hwaddr, dev->adapter->mac_addr, UK_NETDEV_MAC_ADDR_LEN);
	info->features = UK_NETDEV_F_RX_CSUM | UK_NETDEV_F_TX_CSUM;

	return 0;
}

static int ena_netdev_configure(struct uk_netdev *dev, const struct uk_netdev_conf *conf)
{
	if (!dev || !conf || !dev->adapter)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	if (conf->nb_rx_queues == 0 || conf->nb_rx_queues > dev->adapter->max_rx_queues)
		return -EINVAL;

	if (conf->nb_tx_queues == 0 || conf->nb_tx_queues > dev->adapter->max_tx_queues)
		return -EINVAL;

	if (!dev->adapter->rx_rings) {
		dev->adapter->rx_rings = calloc(conf->nb_rx_queues, sizeof(struct ena_ring *));
		if (!dev->adapter->rx_rings)
			return -ENOMEM;
		dev->adapter->num_rx_rings = conf->nb_rx_queues;
	}

	if (!dev->adapter->tx_rings) {
		dev->adapter->tx_rings = calloc(conf->nb_tx_queues, sizeof(struct ena_ring *));
		if (!dev->adapter->tx_rings)
			return -ENOMEM;
		dev->adapter->num_tx_rings = conf->nb_tx_queues;
	}

	dev->nb_rx_queues = conf->nb_rx_queues;
	dev->nb_tx_queues = conf->nb_tx_queues;
	dev->state = UK_NETDEV_CONFIGURED;

	return 0;
}

static int ena_netdev_rxq_configure(struct uk_netdev *dev, uint16_t queue_id,
				    uint16_t nb_desc, const struct uk_netdev_rxqueue_conf *conf)
{
	struct ena_ring *ring = NULL;
	int ret;

	(void)conf;

	if (!dev || !dev->adapter || queue_id >= dev->nb_rx_queues)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	if (nb_desc == 0)
		nb_desc = dev->adapter->max_rx_ring_size;

	if (dev->adapter->rx_rings && dev->adapter->rx_rings[queue_id]) {
		ena_ring_free(dev->adapter->rx_rings[queue_id]);
		dev->adapter->rx_rings[queue_id] = NULL;
	}

	ret = ena_ring_alloc(dev->adapter, queue_id, ENA_RING_TYPE_RX,
			     nb_desc, nb_desc, &ring);
	if (ret)
		return ret;

	if (dev->adapter->rx_rings)
		dev->adapter->rx_rings[queue_id] = ring;

	return 0;
}

static int ena_netdev_txq_configure(struct uk_netdev *dev, uint16_t queue_id,
				    uint16_t nb_desc, const struct uk_netdev_txqueue_conf *conf)
{
	struct ena_ring *ring = NULL;
	int ret;

	(void)conf;

	if (!dev || !dev->adapter || queue_id >= dev->nb_tx_queues)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	if (nb_desc == 0)
		nb_desc = dev->adapter->max_tx_ring_size;

	if (dev->adapter->tx_rings && dev->adapter->tx_rings[queue_id]) {
		ena_ring_free(dev->adapter->tx_rings[queue_id]);
		dev->adapter->tx_rings[queue_id] = NULL;
	}

	ret = ena_ring_alloc(dev->adapter, queue_id, ENA_RING_TYPE_TX,
			     nb_desc, nb_desc, &ring);
	if (ret)
		return ret;

	if (dev->adapter->tx_rings)
		dev->adapter->tx_rings[queue_id] = ring;

	return 0;
}

static int ena_netdev_start(struct uk_netdev *dev)
{
	uint16_t rx_created = 0;
	uint16_t tx_created = 0;
	uint16_t q;
	int ret;

	if (!dev || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_CONFIGURED && dev->state != UK_NETDEV_STOPPED)
		return -EINVAL;

	/* Create hardware queues for all configured RX rings */
	for (q = 0; q < dev->nb_rx_queues; q++) {
		if (dev->adapter->rx_rings && dev->adapter->rx_rings[q]) {
			uint32_t vector = (dev->adapter->irq_vectors) ? q : 0;
			ret = ena_ring_create_hw(dev->adapter->rx_rings[q], vector);
			if (ret)
				goto err_rollback;
			rx_created++;
		}
	}

	/* Create hardware queues for all configured TX rings */
	for (q = 0; q < dev->nb_tx_queues; q++) {
		if (dev->adapter->tx_rings && dev->adapter->tx_rings[q]) {
			uint32_t vector = (dev->adapter->irq_vectors) ? q : 0;
			ret = ena_ring_create_hw(dev->adapter->tx_rings[q], vector);
			if (ret)
				goto err_rollback;
			tx_created++;
		}
	}

	dev->state = UK_NETDEV_RUNNING;
	return 0;

err_rollback:
	/* Roll back all created queues on error */
	for (q = 0; q < tx_created; q++) {
		if (dev->adapter->tx_rings && dev->adapter->tx_rings[q])
			ena_ring_destroy_hw(dev->adapter->tx_rings[q]);
	}
	for (q = 0; q < rx_created; q++) {
		if (dev->adapter->rx_rings && dev->adapter->rx_rings[q])
			ena_ring_destroy_hw(dev->adapter->rx_rings[q]);
	}
	return ret;
}

static int ena_netdev_stop(struct uk_netdev *dev)
{
	uint16_t q;

	if (!dev || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return 0;

	/* Quiesce device: drain in-flight TX completions */
	for (q = 0; q < dev->nb_tx_queues; q++) {
		if (dev->adapter->tx_rings && dev->adapter->tx_rings[q])
			ena_tx_poll_completions(dev->adapter->tx_rings[q], 0, NULL);
	}

	/* Destroy hardware queues */
	for (q = 0; q < dev->nb_rx_queues; q++) {
		if (dev->adapter->rx_rings && dev->adapter->rx_rings[q])
			ena_ring_destroy_hw(dev->adapter->rx_rings[q]);
	}

	for (q = 0; q < dev->nb_tx_queues; q++) {
		if (dev->adapter->tx_rings && dev->adapter->tx_rings[q])
			ena_ring_destroy_hw(dev->adapter->tx_rings[q]);
	}

	dev->state = UK_NETDEV_STOPPED;
	return 0;
}

static int ena_netdev_rxq_recv(struct uk_netdev *dev, uint16_t queue_id,
			       struct uk_netbuf **pkt)
{
	struct ena_rx_pkt rx_pkt;
	struct ena_ring *ring;
	int ret;

	if (!dev || !pkt || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return -EAGAIN;

	if (queue_id >= dev->nb_rx_queues)
		return -EINVAL;

	if (!dev->adapter->rx_rings || !dev->adapter->rx_rings[queue_id])
		return -EINVAL;

	ring = dev->adapter->rx_rings[queue_id];
	ret = ena_rx_poll(ring, &rx_pkt, 1);
	if (ret <= 0)
		return ret;

	if (rx_pkt.netbuf) {
		struct uk_netbuf *nb = (struct uk_netbuf *)rx_pkt.netbuf;
		nb->len = rx_pkt.len;
		nb->hash = rx_pkt.hash;
		nb->l3_csum_err = rx_pkt.l3_csum_err;
		nb->l4_csum_err = rx_pkt.l4_csum_err;
		nb->l4_csum_checked = rx_pkt.l4_csum_checked;
		*pkt = nb;
	}

	return 1;
}

static int ena_netdev_txq_xmit(struct uk_netdev *dev, uint16_t queue_id,
			       struct uk_netbuf *pkt)
{
	struct ena_tx_pkt tx_pkt;
	struct ena_ring *ring;
	int ret;

	if (!dev || !pkt || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return -EAGAIN;

	if (queue_id >= dev->nb_tx_queues)
		return -EINVAL;

	if (!dev->adapter->tx_rings || !dev->adapter->tx_rings[queue_id])
		return -EINVAL;

	ring = dev->adapter->tx_rings[queue_id];

	/* Poll completions to free up space */
	ena_tx_poll_completions(ring, 16, NULL);

	memset(&tx_pkt, 0, sizeof(tx_pkt));
	tx_pkt.netbuf = pkt;
	tx_pkt.phys_addr = pkt->phys_addr;
	tx_pkt.len = (uint32_t)pkt->len;
	tx_pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
	tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
	tx_pkt.l3_csum_en = true;
	tx_pkt.l4_csum_en = true;

	if (pkt->data && pkt->len >= 14) {
		const uint8_t *data = (const uint8_t *)pkt->data;
		uint16_t ethertype = (uint16_t)(((uint16_t)data[12] << 8) | data[13]);
		size_t l3_off = 14;

		if (ethertype == 0x8100 && pkt->len >= 18) {
			ethertype = (uint16_t)(((uint16_t)data[16] << 8) | data[17]);
			l3_off = 18;
		}

		if (ethertype == 0x0800) {
			tx_pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
			tx_pkt.l3_csum_en = true;
			if (pkt->len >= l3_off + 20) {
				uint8_t proto = data[l3_off + 9];
				if (proto == 6) {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
					tx_pkt.l4_csum_en = true;
				} else if (proto == 17) {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
					tx_pkt.l4_csum_en = true;
				} else {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_UNKNOWN;
					tx_pkt.l4_csum_en = false;
				}
			}
		} else if (ethertype == 0x86DD) {
			tx_pkt.l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
			tx_pkt.l3_csum_en = false;
			if (pkt->len >= l3_off + 40) {
				uint8_t proto = data[l3_off + 6];
				if (proto == 6) {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
					tx_pkt.l4_csum_en = true;
				} else if (proto == 17) {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
					tx_pkt.l4_csum_en = true;
				} else {
					tx_pkt.l4_proto = ENA_ETH_IO_L4_PROTO_UNKNOWN;
					tx_pkt.l4_csum_en = false;
				}
			}
		}
	}

	ret = ena_tx_submit(ring, &tx_pkt, NULL);
	if (ret == 0)
		ena_tx_doorbell(ring);

	return ret;
}

static const struct uk_netdev_ops ena_ops = {
	.info_get      = ena_netdev_info_get,
	.configure     = ena_netdev_configure,
	.rxq_configure = ena_netdev_rxq_configure,
	.txq_configure = ena_netdev_txq_configure,
	.dev_start     = ena_netdev_start,
	.dev_stop      = ena_netdev_stop,
	.rxq_recv      = ena_netdev_rxq_recv,
	.txq_xmit      = ena_netdev_txq_xmit,
};

struct uk_netdev *ena_netdev_alloc(struct ena_adapter *adapter)
{
	struct uk_netdev *netdev;

	if (!adapter)
		return NULL;

	netdev = calloc(1, sizeof(*netdev));
	if (!netdev)
		return NULL;

	netdev->adapter = adapter;
	netdev->state = UK_NETDEV_UNCONFIGURED;
	netdev->ops = &ena_ops;

	return netdev;
}

void ena_netdev_free(struct uk_netdev *netdev)
{
	uint16_t q, count;

	if (!netdev)
		return;

	if (netdev->adapter) {
		struct ena_adapter *adapter = netdev->adapter;

		if (adapter->rx_rings) {
			count = adapter->num_rx_rings ? adapter->num_rx_rings : adapter->max_rx_queues;
			for (q = 0; q < count; q++) {
				if (adapter->rx_rings[q]) {
					ena_ring_free(adapter->rx_rings[q]);
					adapter->rx_rings[q] = NULL;
				}
			}
			free(adapter->rx_rings);
			adapter->rx_rings = NULL;
			adapter->num_rx_rings = 0;
		}

		if (adapter->tx_rings) {
			count = adapter->num_tx_rings ? adapter->num_tx_rings : adapter->max_tx_queues;
			for (q = 0; q < count; q++) {
				if (adapter->tx_rings[q]) {
					ena_ring_free(adapter->tx_rings[q]);
					adapter->tx_rings[q] = NULL;
				}
			}
			free(adapter->tx_rings);
			adapter->tx_rings = NULL;
			adapter->num_tx_rings = 0;
		}
	}

	free(netdev);
}

int ena_netdev_register(struct uk_netdev *netdev)
{
	if (!netdev || !netdev->adapter)
		return -EINVAL;

	netdev->ops = &ena_ops;
	return 0;
}
