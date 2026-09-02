/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_netdev.h"
#include "ena_datapath.h"
#include "ena_intr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Shared Datapath and Netdev Helper Functions
 * ------------------------------------------------------------------------- */

/* Maximum AENQ events drained per datapath poll iteration. */
#define ENA_NETDEV_AENQ_POLL_BUDGET	8

/* Drain pending asynchronous events from the AENQ ring.
 * The driver dispatches them to the registered handler (default handler
 * registered at probe time). Called on every datapath poll iteration.
 * Returns the number of dispatched events, or a negative errno value. */
static int ena_netdev_drain_aenq(struct ena_adapter *adapter)
{
	return ena_admin_aenq_poll(adapter, ENA_NETDEV_AENQ_POLL_BUDGET);
}

/* Clamp descriptor count within driver and hardware limits */
static uint16_t ena_netdev_clamp_desc_count(uint16_t nb_desc, uint16_t max_ring_size)
{
	if (nb_desc == 0)
		nb_desc = max_ring_size ? max_ring_size : ENA_DEFAULT_RING_DESC;

	if (max_ring_size && nb_desc > max_ring_size)
		nb_desc = max_ring_size;
	if (nb_desc > ENA_MAX_RING_DESC)
		nb_desc = ENA_MAX_RING_DESC;
	if (nb_desc < ENA_MIN_RING_DESC)
		nb_desc = ENA_MIN_RING_DESC;

	return nb_desc;
}

/* Allocate ring pointer arrays in adapter */
static int ena_netdev_alloc_ring_arrays(struct ena_adapter *adapter,
				       uint16_t nb_rx_queues,
				       uint16_t nb_tx_queues)
{
	if (!adapter)
		return -EINVAL;

	if (!adapter->rx_rings) {
		adapter->rx_rings = calloc(nb_rx_queues, sizeof(struct ena_ring *));
		if (!adapter->rx_rings)
			return -ENOMEM;
		adapter->num_rx_rings = nb_rx_queues;
	}

	if (!adapter->tx_rings) {
		adapter->tx_rings = calloc(nb_tx_queues, sizeof(struct ena_ring *));
		if (!adapter->tx_rings)
			return -ENOMEM;
		adapter->num_tx_rings = nb_tx_queues;
	}

	return 0;
}

/* Create hardware queues for all configured rings with rollback on error */
static int ena_netdev_start_rings_hw(struct ena_adapter *adapter,
				     uint16_t nb_rx, uint16_t nb_tx)
{
	uint16_t rx_created = 0;
	uint16_t tx_created = 0;
	uint16_t q;
	int ret;

	if (!adapter)
		return -EINVAL;

	/* Create hardware queues for TX rings */
	for (q = 0; q < nb_tx; q++) {
		if (adapter->tx_rings && adapter->tx_rings[q]) {
			uint32_t vector = (adapter->irq_vectors) ? q : 0;
			ret = ena_ring_create_hw(adapter->tx_rings[q], vector);
			if (ret)
				goto err_rollback;
			tx_created++;
		}
	}

	/* Create hardware queues for RX rings */
	for (q = 0; q < nb_rx; q++) {
		if (adapter->rx_rings && adapter->rx_rings[q]) {
			uint32_t vector = (adapter->irq_vectors) ? q : 0;
			ret = ena_ring_create_hw(adapter->rx_rings[q], vector);
			if (ret)
				goto err_rollback;
			rx_created++;
		}
	}

	return 0;

err_rollback:
	/* Roll back all created queues on error */
	for (q = 0; q < rx_created; q++) {
		if (adapter->rx_rings && adapter->rx_rings[q])
			ena_ring_destroy_hw(adapter->rx_rings[q]);
	}
	for (q = 0; q < tx_created; q++) {
		if (adapter->tx_rings && adapter->tx_rings[q])
			ena_ring_destroy_hw(adapter->tx_rings[q]);
	}
	return ret;
}

/* Stop hardware queues and drain in-flight TX completions */
static int ena_netdev_stop_rings_hw(struct ena_adapter *adapter,
				    uint16_t nb_rx, uint16_t nb_tx)
{
	uint16_t q;

	if (!adapter)
		return -EINVAL;

	/* Drain in-flight TX completions */
	for (q = 0; q < nb_tx; q++) {
		if (adapter->tx_rings && adapter->tx_rings[q])
			ena_tx_poll_completions(adapter->tx_rings[q], 0, NULL);
	}

	/* Destroy hardware queues */
	for (q = 0; q < nb_rx; q++) {
		if (adapter->rx_rings && adapter->rx_rings[q])
			ena_ring_destroy_hw(adapter->rx_rings[q]);
	}

	for (q = 0; q < nb_tx; q++) {
		if (adapter->tx_rings && adapter->tx_rings[q])
			ena_ring_destroy_hw(adapter->tx_rings[q]);
	}

	return 0;
}

/* Free all software rings attached to adapter */
static void ena_netdev_cleanup_adapter_rings(struct ena_adapter *adapter)
{
	uint16_t q, count;

	if (!adapter)
		return;

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

/* Inspect packet payload and classify L3 and L4 protocols */
static void ena_netdev_classify_tx_pkt(const struct uk_netbuf *pkt, struct ena_tx_pkt *tx_pkt)
{
	tx_pkt->l3_proto = ENA_ETH_IO_L3_PROTO_UNKNOWN;
	tx_pkt->l4_proto = ENA_ETH_IO_L4_PROTO_UNKNOWN;
	tx_pkt->l3_csum_en = false;
	tx_pkt->l4_csum_en = false;

	if (!pkt || !pkt->data || pkt->len < 14)
		return;

	const uint8_t *data = (const uint8_t *)pkt->data;
	uint16_t ethertype = (uint16_t)(((uint16_t)data[12] << 8) | data[13]);
	size_t l3_off = 14;

	if (ethertype == 0x8100 && pkt->len >= 18) {
		ethertype = (uint16_t)(((uint16_t)data[16] << 8) | data[17]);
		l3_off = 18;
	}

	if (ethertype == 0x0800) {
		tx_pkt->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
		if (pkt->len >= l3_off + 20) {
			uint8_t proto = data[l3_off + 9];
			if (proto == 6)
				tx_pkt->l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
			else if (proto == 17)
				tx_pkt->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
		}
	} else if (ethertype == 0x86DD) {
		tx_pkt->l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
		if (pkt->len >= l3_off + 40) {
			uint8_t proto = data[l3_off + 6];
			if (proto == 6)
				tx_pkt->l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
			else if (proto == 17)
				tx_pkt->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
		}
	}

	/* Selectively enable hardware checksum offload if requested */
	if (pkt->flags & UK_NETBUF_F_PARTIAL_CSUM) {
		if (tx_pkt->l3_proto == ENA_ETH_IO_L3_PROTO_IPV4)
			tx_pkt->l3_csum_en = true;
		if (tx_pkt->l4_proto == ENA_ETH_IO_L4_PROTO_TCP || tx_pkt->l4_proto == ENA_ETH_IO_L4_PROTO_UDP)
			tx_pkt->l4_csum_en = true;
	}
	if (pkt->flags & UK_NETBUF_F_GSO_TCPV4)
		tx_pkt->tso_en = true;
}

/* Free RX queue bounce buffer and tracking metadata */
static void ena_netdev_free_rxq_bounce(struct uk_netdev_rx_queue *rxq)
{
	if (!rxq)
		return;

	if (rxq->bounce_buf) {
		ena_dma_free(rxq->bounce_buf, rxq->bounce_phys);
		rxq->bounce_buf = NULL;
		rxq->bounce_phys = 0;
	}

	if (rxq->bounce_free_ids) {
		free(rxq->bounce_free_ids);
		rxq->bounce_free_ids = NULL;
	}

	if (rxq->bounce_map) {
		free(rxq->bounce_map);
		rxq->bounce_map = NULL;
	}

	rxq->bounce_free_head = 0;
	rxq->bounce_free_tail = 0;
	rxq->bounce_free_count = 0;
	rxq->pending_slot = -1;
	rxq->nb_desc = 0;
}

/* Free TX queue bounce buffer */
static void ena_netdev_free_txq_bounce(struct uk_netdev_tx_queue *txq)
{
	if (!txq)
		return;

	if (txq->bounce_buf) {
		ena_dma_free(txq->bounce_buf, txq->bounce_phys);
		txq->bounce_buf = NULL;
		txq->bounce_phys = 0;
	}

	txq->bounce_in_use = false;
	txq->bounce_req_id = 0;
	txq->bounce_wait_polls = 0;
	txq->nb_desc = 0;
}

/* Release a TX bounce buffer whose completion never arrived. The request is
 * treated as lost: the in-flight flag is cleared, the request id is returned
 * to the ring free pool, and the bounce is available for the next transmit.
 * Callers must hold no other claim on the ring; the ring lock is taken here. */
static void ena_netdev_release_stuck_tx_bounce(struct ena_ring *ring,
					       struct uk_netdev_tx_queue *txq)
{
	ena_ring_lock(ring);

	if (ring->req_in_flight && txq->bounce_req_id < ring->sq_depth &&
	    ring->req_in_flight[txq->bounce_req_id]) {
		ring->req_in_flight[txq->bounce_req_id] = 0;
		ena_ring_req_id_free(ring, txq->bounce_req_id);
	}

	ena_ring_unlock(ring);

	txq->bounce_in_use = false;
	txq->bounce_req_id = 0;
	txq->bounce_wait_polls = 0;
}

/* Return a dropped RX netbuf bounce slot to the queue pool and free the netbuf */
static void ena_netdev_rxq_drop_netbuf(void *arg, void *netbuf)
{
	struct uk_netdev_rx_queue *rxq = (struct uk_netdev_rx_queue *)arg;
	struct uk_netbuf *nb = (struct uk_netbuf *)netbuf;

	if (!rxq || !nb)
		return;

#ifdef __Unikraft__
	uk_netbuf_free(nb);
#endif
}


/* Submit one prepared TX packet on a ring. If the ring uses a device
 * LLQ push buffer, the descriptor and the inline header go to BAR2 and
 * the doorbell is rung inside ena_llq_tx_push(). Otherwise the standard
 * host-memory submission path is used. */
static int ena_netdev_tx_xmit_one(struct ena_ring *ring,
				  const struct ena_tx_pkt *tx_pkt,
				  const void *hdr_data, uint16_t *out_req_id)
{
	if (ring->is_llq) {
		uint32_t max_hdr = ring->llq_header_len ?
				   ring->llq_header_len : 96;
		uint16_t hdr_len = (uint16_t)(tx_pkt->len < max_hdr ?
					      tx_pkt->len : max_hdr);

		return ena_llq_tx_push(ring, tx_pkt, hdr_data, hdr_len,
				       out_req_id);
	}

	{
		int ret = ena_tx_submit(ring, tx_pkt, out_req_id);

		if (ret == 0)
			ena_tx_doorbell(ring);
		return ret;
	}
}



#ifdef __Unikraft__


static void ena_netdev_info_get(struct uk_netdev *dev, struct uk_netdev_info *info)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;

	info->max_rx_queues = adapter->max_rx_queues ? adapter->max_rx_queues : 1;
	info->max_tx_queues = adapter->max_tx_queues ? adapter->max_tx_queues : 1;
	info->max_mtu = adapter->max_mtu ? adapter->max_mtu : ENA_DEFAULT_MTU;
	info->nb_encap_tx = 0;
	info->nb_encap_rx = 0;
	info->ioalign = ENA_NETDEV_IOALIGN;
	info->in_queue_pairs = 1;
	info->features = UK_NETDEV_F_PARTIAL_CSUM | UK_NETDEV_F_LRO | UK_NETDEV_F_TSO4;
}

static int ena_netdev_rxq_info_get(struct uk_netdev *dev, uint16_t queue_id __attribute__((unused)),
				   struct uk_netdev_queue_info *queue_info)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	queue_info->nb_min = 32;
	queue_info->nb_max = edev->adapter.max_rx_ring_size ? edev->adapter.max_rx_ring_size : ENA_DEFAULT_RING_DESC;
	queue_info->nb_align = 1;
	queue_info->nb_is_power_of_two = 1;
	return 0;
}

static int ena_netdev_txq_info_get(struct uk_netdev *dev, uint16_t queue_id __attribute__((unused)),
				   struct uk_netdev_queue_info *queue_info)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	queue_info->nb_min = 32;
	queue_info->nb_max = edev->adapter.max_tx_ring_size ? edev->adapter.max_tx_ring_size : ENA_DEFAULT_RING_DESC;
	queue_info->nb_align = 1;
	queue_info->nb_is_power_of_two = 1;
	return 0;
}

static unsigned ena_netdev_promisc_get(struct uk_netdev *dev __attribute__((unused)))
{
	return 0;
}

static const struct uk_hwaddr *ena_netdev_mac_get(struct uk_netdev *dev)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	return (const struct uk_hwaddr *)edev->adapter.mac_addr;
}

static uint16_t ena_netdev_mtu_get(struct uk_netdev *dev)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	return edev->adapter.mtu ? edev->adapter.mtu : ENA_DEFAULT_MTU;
}

bool ena_netdev_link_get(struct uk_netdev *dev)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	return edev->adapter.link_up;
}

static int ena_netdev_configure(struct uk_netdev *dev, const struct uk_netdev_conf *conf)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;

	if (!conf)
		return -EINVAL;

	/* Reject reconfiguration while the device is running. */
	if (uk_netdev_state_get(dev) == UK_NETDEV_RUNNING)
		return -EBUSY;

	if (conf->nb_rx_queues == 0 || (adapter->max_rx_queues && conf->nb_rx_queues > adapter->max_rx_queues) || conf->nb_rx_queues > ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (conf->nb_tx_queues == 0 || (adapter->max_tx_queues && conf->nb_tx_queues > adapter->max_tx_queues) || conf->nb_tx_queues > ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	return ena_netdev_alloc_ring_arrays(adapter, conf->nb_rx_queues, conf->nb_tx_queues);
}

static struct uk_netdev_rx_queue *ena_netdev_rxq_configure(struct uk_netdev *dev, uint16_t queue_id,
							    uint16_t nb_desc, struct uk_netdev_rxqueue_conf *conf)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;
	struct ena_ring *ring = NULL;
	uint16_t i;
	int ret;

	if (queue_id >= ENA_NETDEV_MAX_QUEUES || (adapter->num_rx_rings && queue_id >= adapter->num_rx_rings))
		return NULL;

	/* Reject reconfiguration while the device is running. */
	if (uk_netdev_state_get(dev) == UK_NETDEV_RUNNING)
		return NULL;

	ena_info("rxq_configure: qid=%u nb_desc=%u max_rx_ring_size=%u",
		 queue_id, nb_desc, adapter->max_rx_ring_size);

	nb_desc = ena_netdev_clamp_desc_count(nb_desc, adapter->max_rx_ring_size);

	/* Release previous resources if reconfigured */
	if (adapter->rx_rings && adapter->rx_rings[queue_id]) {
		ena_ring_free(adapter->rx_rings[queue_id]);
		adapter->rx_rings[queue_id] = NULL;
	}
	ena_netdev_free_rxq_bounce(&edev->rx_queues[queue_id]);

	ret = ena_ring_alloc(adapter, queue_id, ENA_RING_TYPE_RX, nb_desc, nb_desc, &ring);
	if (ret)
		return NULL;

	if (adapter->rx_rings)
		adapter->rx_rings[queue_id] = ring;

	edev->rx_queues[queue_id].ring = ring;
	edev->rx_queues[queue_id].queue_id = queue_id;
	edev->rx_queues[queue_id].adapter = adapter;
	edev->rx_queues[queue_id].allocator = conf ? conf->a : NULL;
	edev->rx_queues[queue_id].alloc_rxpkts = conf ? conf->alloc_rxpkts : NULL;
	edev->rx_queues[queue_id].alloc_rxpkts_argp = conf ? conf->alloc_rxpkts_argp : NULL;
	edev->rx_queues[queue_id].nb_desc = nb_desc;

	ring->drop_netbuf_cb = ena_netdev_rxq_drop_netbuf;
	ring->drop_netbuf_arg = &edev->rx_queues[queue_id];

	/* Allocate per-slot bounce buffers for low memory descriptors */
	edev->rx_queues[queue_id].bounce_buf = ena_dma_alloc((size_t)nb_desc * ENA_RX_BUF_SIZE,
							     &edev->rx_queues[queue_id].bounce_phys);
	if (!edev->rx_queues[queue_id].bounce_buf) {
		ena_ring_free(ring);
		if (adapter->rx_rings)
			adapter->rx_rings[queue_id] = NULL;
		return NULL;
	}

	edev->rx_queues[queue_id].bounce_free_ids = calloc(nb_desc, sizeof(uint16_t));
	if (!edev->rx_queues[queue_id].bounce_free_ids) {
		ena_netdev_free_rxq_bounce(&edev->rx_queues[queue_id]);
		ena_ring_free(ring);
		if (adapter->rx_rings)
			adapter->rx_rings[queue_id] = NULL;
		return NULL;
	}

	edev->rx_queues[queue_id].bounce_map = malloc(nb_desc * sizeof(int16_t));
	if (!edev->rx_queues[queue_id].bounce_map) {
		ena_netdev_free_rxq_bounce(&edev->rx_queues[queue_id]);
		ena_ring_free(ring);
		if (adapter->rx_rings)
			adapter->rx_rings[queue_id] = NULL;
		return NULL;
	}

	for (i = 0; i < nb_desc; i++) {
		edev->rx_queues[queue_id].bounce_free_ids[i] = i;
		edev->rx_queues[queue_id].bounce_map[i] = -1;
	}
	edev->rx_queues[queue_id].bounce_free_head = 0;
	edev->rx_queues[queue_id].bounce_free_tail = 0;
	edev->rx_queues[queue_id].bounce_free_count = nb_desc;
	edev->rx_queues[queue_id].pending_slot = -1;

	return &edev->rx_queues[queue_id];
}

static struct uk_netdev_tx_queue *ena_netdev_txq_configure(struct uk_netdev *dev, uint16_t queue_id,
							    uint16_t nb_desc, struct uk_netdev_txqueue_conf *conf)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;
	struct ena_ring *ring = NULL;
	int ret;

	(void)conf;

	if (queue_id >= ENA_NETDEV_MAX_QUEUES || (adapter->num_tx_rings && queue_id >= adapter->num_tx_rings))
		return NULL;

	/* Reject reconfiguration while the device is running. */
	if (uk_netdev_state_get(dev) == UK_NETDEV_RUNNING)
		return NULL;

	ena_info("txq_configure: qid=%u nb_desc=%u max_tx_ring_size=%u",
		 queue_id, nb_desc, adapter->max_tx_ring_size);

	nb_desc = ena_netdev_clamp_desc_count(nb_desc, adapter->max_tx_ring_size);

	/* Release previous resources if reconfigured */
	if (adapter->tx_rings && adapter->tx_rings[queue_id]) {
		ena_ring_free(adapter->tx_rings[queue_id]);
		adapter->tx_rings[queue_id] = NULL;
	}
	ena_netdev_free_txq_bounce(&edev->tx_queues[queue_id]);

	ret = ena_ring_alloc(adapter, queue_id, ENA_RING_TYPE_TX, nb_desc, nb_desc, &ring);
	if (ret)
		return NULL;

	if (adapter->tx_rings)
		adapter->tx_rings[queue_id] = ring;

	edev->tx_queues[queue_id].ring = ring;
	edev->tx_queues[queue_id].queue_id = queue_id;
	edev->tx_queues[queue_id].adapter = adapter;
	edev->tx_queues[queue_id].bounce_in_use = false;
	edev->tx_queues[queue_id].bounce_req_id = 0;
	edev->tx_queues[queue_id].bounce_wait_polls = 0;
	edev->tx_queues[queue_id].nb_desc = nb_desc;

	edev->tx_queues[queue_id].bounce_buf = ena_dma_alloc(ENA_TX_BOUNCE_SIZE,
							     &edev->tx_queues[queue_id].bounce_phys);
	if (!edev->tx_queues[queue_id].bounce_buf) {
		ena_ring_free(ring);
		if (adapter->tx_rings)
			adapter->tx_rings[queue_id] = NULL;
		return NULL;
	}

	return &edev->tx_queues[queue_id];
}

static void *ena_netbuf_alloc_helper(void *arg, uint64_t *phys_out, uint32_t *len_out)
{
	struct uk_netdev_rx_queue *rxq = (struct uk_netdev_rx_queue *)arg;
	struct uk_netbuf *nb = NULL;
	uint64_t phys;

	if (rxq && rxq->alloc_rxpkts) {
		uint16_t n = rxq->alloc_rxpkts(rxq->alloc_rxpkts_argp, &nb, 1);
		if (n == 0 || !nb)
			return NULL;
	} else {
		struct uk_alloc *a = rxq ? rxq->allocator : uk_alloc_get_default();
		nb = uk_netbuf_alloc_buf(a, ENA_RX_BUF_SIZE, ENA_NETDEV_IOALIGN, 0, 0, NULL);
		if (!nb)
			return NULL;
	}

	phys = (uint64_t)(uintptr_t)nb->data;
	if (phys < ENA_DMA_LOW_MEM_LIMIT && rxq && rxq->bounce_buf) {
		if (rxq->bounce_free_count == 0) {
			/* No free bounce slots available */
#ifdef __Unikraft__
			uk_netbuf_free(nb);
#endif
			return NULL;
		}

		uint16_t slot = rxq->bounce_free_ids[rxq->bounce_free_head];
		rxq->bounce_free_head = (uint16_t)((rxq->bounce_free_head + 1) & (rxq->nb_desc - 1));
		rxq->bounce_free_count--;

		if (phys_out)
			*phys_out = rxq->bounce_phys + ((uint64_t)slot * ENA_RX_BUF_SIZE);
		if (len_out) {
			/* Never offer more than the netbuf can hold */
			*len_out = (nb->buflen < ENA_RX_BUF_SIZE) ? (uint32_t)nb->buflen
			                                       : (uint32_t)ENA_RX_BUF_SIZE;
		}
	} else {
		if (phys_out)
			*phys_out = phys;
		if (len_out)
			*len_out = (uint32_t)nb->buflen;
	}

	return nb;
}

static int ena_netdev_start(struct uk_netdev *dev)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;
	uint16_t q;
	int ret;

	ret = ena_netdev_start_rings_hw(adapter, adapter->num_rx_rings, adapter->num_tx_rings);
	if (ret)
		return ret;

	for (q = 0; q < adapter->num_rx_rings; q++) {
		if (adapter->rx_rings && adapter->rx_rings[q]) {
			ena_rx_refill(adapter->rx_rings[q], adapter->rx_rings[q]->sq_depth - 1,
				      ena_netbuf_alloc_helper, &edev->rx_queues[q], NULL);
		}
	}

	return 0;
}

static int ena_netdev_stop(struct uk_netdev *dev)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_adapter *adapter = &edev->adapter;

	return ena_netdev_stop_rings_hw(adapter, adapter->num_rx_rings, adapter->num_tx_rings);
}

int ena_netdev_rx_one(struct uk_netdev *dev,
		      struct uk_netdev_rx_queue *queue,
		      struct uk_netbuf **pkt)
{
	struct ena_uk_device *edev = to_enadevice(dev);
	struct ena_rx_pkt rx_pkt;
	struct ena_ring *ring;
	int ret;

	if (!queue || !queue->ring || !pkt)
		return -EINVAL;

	/* Drain asynchronous events on every poll iteration. */
	ena_netdev_drain_aenq(&edev->adapter);

	ring = queue->ring;

	while (1) {
		ret = ena_rx_poll(ring, &rx_pkt, 1);
		if (ret <= 0)
			return 0;

		if (!rx_pkt.netbuf)
			return 0;

		struct uk_netbuf *nb = (struct uk_netbuf *)rx_pkt.netbuf;
		int16_t slot = (queue->bounce_map && rx_pkt.req_id < queue->nb_desc) ?
			       queue->bounce_map[rx_pkt.req_id] : -1;
		bool dropped = false;

		nb->len = rx_pkt.len;
		nb->next = NULL;

		/* Set DATA_VALID if hardware validated L3 & L4 checksums */
		if (!rx_pkt.l3_csum_err && !rx_pkt.l4_csum_err && rx_pkt.l4_csum_checked)
			nb->flags |= UK_NETBUF_F_DATA_VALID;

		/* Copy payload if received into a low memory bounce slot */
		if (slot >= 0) {
			queue->bounce_map[rx_pkt.req_id] = -1;
			if (queue->bounce_buf && (uint16_t)slot < queue->nb_desc && nb->data) {
				void *slot_virt = (char *)queue->bounce_buf + ((size_t)slot * ENA_RX_BUF_SIZE);

				/* Drop the packet if it does not fit the application buffer */
				if (rx_pkt.len <= nb->buflen)
					memcpy(nb->data, slot_virt, rx_pkt.len);
				else
					dropped = true;
			}

			/* Return slot to bounce free pool */
			if (queue->bounce_free_ids && queue->nb_desc > 0) {
				queue->bounce_free_ids[queue->bounce_free_tail] = (uint16_t)slot;
				queue->bounce_free_tail = (uint16_t)((queue->bounce_free_tail + 1) & (queue->nb_desc - 1));
				queue->bounce_free_count++;
			}
		}

		if (dropped) {
			ena_netdev_rxq_drop_netbuf(queue, nb);
			ena_rx_refill(ring, 1, ena_netbuf_alloc_helper, queue, NULL);
			return 0;
		}

		ena_rx_refill(ring, 1, ena_netbuf_alloc_helper, queue, NULL);

		/* Packet reassembly: handle both single-descriptor and multi-descriptor (LRO/jumbo) chains */
		if (!queue->chain_head) {
			if (rx_pkt.first && !rx_pkt.last) {
				/* Start of multi-descriptor frame */
				queue->chain_head = nb;
				queue->chain_tail = nb;
				continue;
			}
			/* Complete single-descriptor frame */
			*pkt = nb;
			return UK_NETDEV_STATUS_SUCCESS;
		} else {
			/* Continuation of multi-descriptor frame */
			queue->chain_tail->next = nb;
			queue->chain_tail = nb;
			if (rx_pkt.last) {
				*pkt = queue->chain_head;
				queue->chain_head = NULL;
				queue->chain_tail = NULL;
				return UK_NETDEV_STATUS_SUCCESS;
			}
			continue;
		}
	}
}

int ena_netdev_tx_one(struct uk_netdev *dev __attribute__((unused)),
		      struct uk_netdev_tx_queue *queue,
		      struct uk_netbuf *pkt)
{
	struct ena_tx_pkt tx_pkt;
	struct ena_ring *ring;
	uint16_t req_id = 0;
	uint64_t phys;
	bool used_bounce = false;
	int ret;

	if (!queue || !queue->ring || !pkt)
		return -EINVAL;

	ring = queue->ring;
	ena_tx_poll_completions(ring, 32, NULL);

	/* Check if previous bounce transmission completed. If it never does
	 * (a lost or stuck completion), release the bounce after a bounded
	 * number of transmit attempts so low-memory transmit is not blocked
	 * forever. */
	if (queue->bounce_in_use) {
		if (!ring->req_in_flight || !ring->req_in_flight[queue->bounce_req_id]) {
			queue->bounce_in_use = false;
			queue->bounce_wait_polls = 0;
		} else if (queue->bounce_wait_polls >= ENA_TX_BOUNCE_STALL_LIMIT) {
			ena_err("tx q%u: bounce completion not seen after %u polls; releasing bounce",
				queue->queue_id, (unsigned)queue->bounce_wait_polls);
			ena_netdev_release_stuck_tx_bounce(ring, queue);
		} else {
			queue->bounce_wait_polls++;
		}
	}

	phys = (uint64_t)(uintptr_t)pkt->data;
	if (phys < ENA_DMA_LOW_MEM_LIMIT) {
		if (queue->bounce_in_use)
			return -EBUSY;

		if (!queue->bounce_buf) {
			queue->bounce_buf = ena_dma_alloc(ENA_TX_BOUNCE_SIZE, &queue->bounce_phys);
			if (!queue->bounce_buf)
				return -ENOMEM;
		}

		if (pkt->len > ENA_TX_BOUNCE_SIZE)
			return -EINVAL;

		memcpy(queue->bounce_buf, pkt->data, pkt->len);
		phys = queue->bounce_phys;
		used_bounce = true;
	}

	memset(&tx_pkt, 0, sizeof(tx_pkt));
	tx_pkt.netbuf = pkt;
	tx_pkt.len = (uint32_t)pkt->len;
	tx_pkt.phys_addr = phys;
	ena_netdev_classify_tx_pkt(pkt, &tx_pkt);

	ret = ena_netdev_tx_xmit_one(ring, &tx_pkt,
				     used_bounce ? queue->bounce_buf :
						     (const void *)pkt->data,
				     &req_id);
	if (ret == 0) {
		if (used_bounce) {
			queue->bounce_in_use = true;
			queue->bounce_req_id = req_id;
			queue->bounce_wait_polls = 0;
		}
		return UK_NETDEV_STATUS_SUCCESS;
	}

	return ret;
}

/*
 * Tear down all driver-owned resources of the device: the hardware
 * queues (SQ and CQ of every ring, LLQ included), the bounce buffers,
 * the software rings, the MSI-X vector table, and the admin queues.
 * Each step is a no-op for a resource that was not allocated, so the
 * sequence is safe after a failed setup and safe to call twice.
 */
void ena_netdev_teardown(struct ena_uk_device *edev)
{
	struct ena_adapter *adapter;
	uint16_t q;

	if (!edev)
		return;

	adapter = &edev->adapter;

	/* The uknetdev API has no stop op. The stop operation drains
	 * the TX queues and destroys the hardware queues before the
	 * rings are released. */
	ena_netdev_stop(&edev->netdev);

	for (q = 0; q < ENA_NETDEV_MAX_QUEUES; q++) {
		ena_netdev_free_rxq_bounce(&edev->rx_queues[q]);
		ena_netdev_free_txq_bounce(&edev->tx_queues[q]);
	}

	ena_netdev_cleanup_adapter_rings(adapter);

	ena_intr_msix_fini(adapter);

	/* Destroy the AQ, ACQ and AENQ and release the host info
	 * buffer. Safe to call twice. */
	ena_admin_fini(adapter);
}

void ena_netdev_free(struct uk_netdev *netdev)
{
	if (!netdev)
		return;

	/* The netdev struct is embedded in the ENA device struct owned
	 * by the PCI layer, so the struct itself is not freed here.
	 * The teardown stops the hardware and releases every
	 * driver-owned resource of the device. */
	ena_netdev_teardown(to_enadevice(netdev));
}

const struct uk_netdev_ops ena_ops = {
	.info_get        = ena_netdev_info_get,
	.rxq_info_get    = ena_netdev_rxq_info_get,
	.txq_info_get    = ena_netdev_txq_info_get,
	.promiscuous_get = ena_netdev_promisc_get,
	.hwaddr_get      = ena_netdev_mac_get,
	.mtu_get         = ena_netdev_mtu_get,
	.configure       = ena_netdev_configure,
	.rxq_configure   = ena_netdev_rxq_configure,
	.txq_configure   = ena_netdev_txq_configure,
	.start           = ena_netdev_start,
};

#else /* !__Unikraft__ (Standalone Test Suite) */

static int ena_netdev_info_get(struct uk_netdev *dev, struct uk_netdev_info *info)
{
	if (!dev || !info || !dev->adapter)
		return -EINVAL;

	info->max_rx_queues = dev->adapter->max_rx_queues;
	info->max_tx_queues = dev->adapter->max_tx_queues;
	info->max_mtu = dev->adapter->max_mtu;
	info->min_mtu = ENA_MIN_MTU_LEN;
	info->mtu = dev->adapter->mtu;
	memcpy(info->hwaddr, dev->adapter->mac_addr, UK_NETDEV_MAC_ADDR_LEN);
	info->features = UK_NETDEV_F_PARTIAL_CSUM | UK_NETDEV_F_LRO | UK_NETDEV_F_TSO4;

	return 0;
}

static int ena_netdev_configure(struct uk_netdev *dev, const struct uk_netdev_conf *conf)
{
	if (!dev || !conf || !dev->adapter)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	if (conf->nb_rx_queues == 0 || conf->nb_rx_queues > dev->adapter->max_rx_queues || conf->nb_rx_queues > ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (conf->nb_tx_queues == 0 || conf->nb_tx_queues > dev->adapter->max_tx_queues || conf->nb_tx_queues > ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	int ret = ena_netdev_alloc_ring_arrays(dev->adapter, conf->nb_rx_queues, conf->nb_tx_queues);
	if (ret)
		return ret;

	dev->nb_rx_queues = conf->nb_rx_queues;
	dev->nb_tx_queues = conf->nb_tx_queues;
	dev->state = UK_NETDEV_CONFIGURED;

	return 0;
}

static int ena_netdev_rxq_configure(struct uk_netdev *dev, uint16_t queue_id,
				    uint16_t nb_desc, const struct uk_netdev_rxqueue_conf *conf)
{
	struct ena_ring *ring = NULL;
	uint16_t i;
	int ret;

	if (!dev || !dev->adapter || queue_id >= dev->nb_rx_queues || queue_id >= ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	nb_desc = ena_netdev_clamp_desc_count(nb_desc, dev->adapter->max_rx_ring_size);

	if (dev->adapter->rx_rings && dev->adapter->rx_rings[queue_id]) {
		ena_ring_free(dev->adapter->rx_rings[queue_id]);
		dev->adapter->rx_rings[queue_id] = NULL;
	}
	ena_netdev_free_rxq_bounce(&dev->rx_queues[queue_id]);

	ret = ena_ring_alloc(dev->adapter, queue_id, ENA_RING_TYPE_RX,
			     nb_desc, nb_desc, &ring);
	if (ret)
		return ret;

	if (dev->adapter->rx_rings)
		dev->adapter->rx_rings[queue_id] = ring;

	dev->rx_queues[queue_id].ring = ring;
	dev->rx_queues[queue_id].queue_id = queue_id;
	dev->rx_queues[queue_id].adapter = dev->adapter;
	dev->rx_queues[queue_id].allocator = conf ? conf->allocator : NULL;
	dev->rx_queues[queue_id].alloc_rxpkts = conf ? conf->alloc_rxpkts : NULL;
	dev->rx_queues[queue_id].alloc_rxpkts_argp = conf ? conf->alloc_rxpkts_argp : NULL;
	dev->rx_queues[queue_id].nb_desc = nb_desc;

	ring->drop_netbuf_cb = ena_netdev_rxq_drop_netbuf;
	ring->drop_netbuf_arg = &dev->rx_queues[queue_id];

	/* Allocate per-slot bounce buffers for low memory descriptors */
	dev->rx_queues[queue_id].bounce_buf = ena_dma_alloc((size_t)nb_desc * ENA_RX_BUF_SIZE,
							    &dev->rx_queues[queue_id].bounce_phys);
	if (!dev->rx_queues[queue_id].bounce_buf) {
		ena_ring_free(ring);
		if (dev->adapter->rx_rings)
			dev->adapter->rx_rings[queue_id] = NULL;
		return -ENOMEM;
	}

	dev->rx_queues[queue_id].bounce_free_ids = calloc(nb_desc, sizeof(uint16_t));
	if (!dev->rx_queues[queue_id].bounce_free_ids) {
		ena_netdev_free_rxq_bounce(&dev->rx_queues[queue_id]);
		ena_ring_free(ring);
		if (dev->adapter->rx_rings)
			dev->adapter->rx_rings[queue_id] = NULL;
		return -ENOMEM;
	}

	dev->rx_queues[queue_id].bounce_map = malloc(nb_desc * sizeof(int16_t));
	if (!dev->rx_queues[queue_id].bounce_map) {
		ena_netdev_free_rxq_bounce(&dev->rx_queues[queue_id]);
		ena_ring_free(ring);
		if (dev->adapter->rx_rings)
			dev->adapter->rx_rings[queue_id] = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < nb_desc; i++) {
		dev->rx_queues[queue_id].bounce_free_ids[i] = i;
		dev->rx_queues[queue_id].bounce_map[i] = -1;
	}
	dev->rx_queues[queue_id].bounce_free_head = 0;
	dev->rx_queues[queue_id].bounce_free_tail = 0;
	dev->rx_queues[queue_id].bounce_free_count = nb_desc;
	dev->rx_queues[queue_id].pending_slot = -1;

	return 0;
}

static int ena_netdev_txq_configure(struct uk_netdev *dev, uint16_t queue_id,
				    uint16_t nb_desc, const struct uk_netdev_txqueue_conf *conf)
{
	struct ena_ring *ring = NULL;
	int ret;

	(void)conf;

	if (!dev || !dev->adapter || queue_id >= dev->nb_tx_queues || queue_id >= ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (dev->state == UK_NETDEV_RUNNING)
		return -EBUSY;

	nb_desc = ena_netdev_clamp_desc_count(nb_desc, dev->adapter->max_tx_ring_size);

	if (dev->adapter->tx_rings && dev->adapter->tx_rings[queue_id]) {
		ena_ring_free(dev->adapter->tx_rings[queue_id]);
		dev->adapter->tx_rings[queue_id] = NULL;
	}
	ena_netdev_free_txq_bounce(&dev->tx_queues[queue_id]);

	ret = ena_ring_alloc(dev->adapter, queue_id, ENA_RING_TYPE_TX,
			     nb_desc, nb_desc, &ring);
	if (ret)
		return ret;

	if (dev->adapter->tx_rings)
		dev->adapter->tx_rings[queue_id] = ring;

	dev->tx_queues[queue_id].ring = ring;
	dev->tx_queues[queue_id].queue_id = queue_id;
	dev->tx_queues[queue_id].adapter = dev->adapter;
	dev->tx_queues[queue_id].bounce_in_use = false;
	dev->tx_queues[queue_id].bounce_req_id = 0;
	dev->tx_queues[queue_id].bounce_wait_polls = 0;
	dev->tx_queues[queue_id].nb_desc = nb_desc;

	dev->tx_queues[queue_id].bounce_buf = ena_dma_alloc(ENA_TX_BOUNCE_SIZE,
							    &dev->tx_queues[queue_id].bounce_phys);
	if (!dev->tx_queues[queue_id].bounce_buf) {
		ena_ring_free(ring);
		if (dev->adapter->tx_rings)
			dev->adapter->tx_rings[queue_id] = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int ena_netdev_start(struct uk_netdev *dev)
{
	int ret;

	if (!dev || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_CONFIGURED && dev->state != UK_NETDEV_STOPPED)
		return -EINVAL;

	ret = ena_netdev_start_rings_hw(dev->adapter, dev->nb_rx_queues, dev->nb_tx_queues);
	if (ret)
		return ret;

	dev->state = UK_NETDEV_RUNNING;
	return 0;
}

static int ena_netdev_stop(struct uk_netdev *dev)
{
	int ret;

	if (!dev || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return 0;

	ret = ena_netdev_stop_rings_hw(dev->adapter, dev->nb_rx_queues, dev->nb_tx_queues);
	if (ret)
		return ret;

	dev->state = UK_NETDEV_STOPPED;
	return 0;
}

static int ena_netdev_rxq_recv(struct uk_netdev *dev, uint16_t queue_id,
			       struct uk_netbuf **pkt)
{
	struct ena_rx_pkt rx_pkt;
	struct ena_ring *ring;
	struct uk_netdev_rx_queue *rxq;
	int ret;

	if (!dev || !pkt || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return -EAGAIN;

	/* Drain asynchronous events on every poll iteration. */
	ena_netdev_drain_aenq(dev->adapter);

	if (queue_id >= dev->nb_rx_queues || queue_id >= ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (!dev->adapter->rx_rings || !dev->adapter->rx_rings[queue_id])
		return -EINVAL;

	ring = dev->adapter->rx_rings[queue_id];
	rxq = &dev->rx_queues[queue_id];

	while (1) {
		ret = ena_rx_poll(ring, &rx_pkt, 1);
		if (ret <= 0)
			return ret;

		if (!rx_pkt.netbuf)
			return 0;

		struct uk_netbuf *nb = (struct uk_netbuf *)rx_pkt.netbuf;
		int16_t slot = (rxq->bounce_map && rx_pkt.req_id < rxq->nb_desc) ?
			       rxq->bounce_map[rx_pkt.req_id] : -1;
		bool dropped = false;

		nb->len = rx_pkt.len;
		nb->hash = rx_pkt.hash;
		nb->l3_csum_err = rx_pkt.l3_csum_err;
		nb->l4_csum_err = rx_pkt.l4_csum_err;
		nb->l4_csum_checked = rx_pkt.l4_csum_checked;
		nb->next = NULL;

		if (!rx_pkt.l3_csum_err && !rx_pkt.l4_csum_err && rx_pkt.l4_csum_checked)
			nb->flags |= UK_NETBUF_F_DATA_VALID;

		if (slot >= 0) {
			rxq->bounce_map[rx_pkt.req_id] = -1;
			if (rxq->bounce_buf && (uint16_t)slot < rxq->nb_desc && nb->data) {
				void *slot_virt = (char *)rxq->bounce_buf + ((size_t)slot * ENA_RX_BUF_SIZE);

				/* Drop the packet if it does not fit the application buffer */
				if (rx_pkt.len <= nb->buflen)
					memcpy(nb->data, slot_virt, rx_pkt.len);
				else
					dropped = true;
			}

			if (rxq->bounce_free_ids && rxq->nb_desc > 0) {
				rxq->bounce_free_ids[rxq->bounce_free_tail] = (uint16_t)slot;
				rxq->bounce_free_tail = (uint16_t)((rxq->bounce_free_tail + 1) & (rxq->nb_desc - 1));
				rxq->bounce_free_count++;
			}
		}

		if (dropped) {
			ena_netdev_rxq_drop_netbuf(rxq, nb);
			return 0;
		}

		/* Packet reassembly: handle both single-descriptor and multi-descriptor (LRO/jumbo) chains */
		if (!rxq->chain_head) {
			if (rx_pkt.first && !rx_pkt.last) {
				/* Start of multi-descriptor frame */
				rxq->chain_head = nb;
				rxq->chain_tail = nb;
				continue;
			}
			/* Complete single-descriptor frame */
			*pkt = nb;
			return 1;
		} else {
			/* Continuation of multi-descriptor frame */
			rxq->chain_tail->next = nb;
			rxq->chain_tail = nb;
			if (rx_pkt.last) {
				*pkt = rxq->chain_head;
				rxq->chain_head = NULL;
				rxq->chain_tail = NULL;
				return 1;
			}
			continue;
		}
	}
}

static int ena_netdev_txq_xmit(struct uk_netdev *dev, uint16_t queue_id,
			       struct uk_netbuf *pkt)
{
	struct ena_tx_pkt tx_pkt;
	struct ena_ring *ring;
	struct uk_netdev_tx_queue *txq;
	uint16_t req_id = 0;
	uint64_t phys;
	bool used_bounce = false;
	int ret;

	if (!dev || !pkt || !dev->adapter)
		return -EINVAL;

	if (dev->state != UK_NETDEV_RUNNING)
		return -EAGAIN;

	if (queue_id >= dev->nb_tx_queues || queue_id >= ENA_NETDEV_MAX_QUEUES)
		return -EINVAL;

	if (!dev->adapter->tx_rings || !dev->adapter->tx_rings[queue_id])
		return -EINVAL;

	ring = dev->adapter->tx_rings[queue_id];
	txq = &dev->tx_queues[queue_id];

	/* Poll completions to free up space */
	ena_tx_poll_completions(ring, 16, NULL);

	/* Check if previous bounce transmission completed. If it never does
	 * (a lost or stuck completion), release the bounce after a bounded
	 * number of transmit attempts so low-memory transmit is not blocked
	 * forever. */
	if (txq->bounce_in_use) {
		if (!ring->req_in_flight || !ring->req_in_flight[txq->bounce_req_id]) {
			txq->bounce_in_use = false;
			txq->bounce_wait_polls = 0;
		} else if (txq->bounce_wait_polls >= ENA_TX_BOUNCE_STALL_LIMIT) {
			ena_err("tx q%u: bounce completion not seen after %u polls; releasing bounce",
				queue_id, (unsigned)txq->bounce_wait_polls);
			ena_netdev_release_stuck_tx_bounce(ring, txq);
		} else {
			txq->bounce_wait_polls++;
		}
	}

	phys = pkt->phys_addr ? pkt->phys_addr : (uint64_t)(uintptr_t)pkt->data;
	if (phys < ENA_DMA_LOW_MEM_LIMIT) {
		if (txq->bounce_in_use)
			return -EBUSY;

		if (!txq->bounce_buf) {
			txq->bounce_buf = ena_dma_alloc(ENA_TX_BOUNCE_SIZE, &txq->bounce_phys);
			if (!txq->bounce_buf)
				return -ENOMEM;
		}

		if (pkt->len > ENA_TX_BOUNCE_SIZE)
			return -EINVAL;

		if (pkt->data)
			memcpy(txq->bounce_buf, pkt->data, pkt->len);
		phys = txq->bounce_phys;
		used_bounce = true;
	}

	memset(&tx_pkt, 0, sizeof(tx_pkt));
	tx_pkt.netbuf = pkt;
	tx_pkt.phys_addr = phys;
	tx_pkt.len = (uint32_t)pkt->len;
	ena_netdev_classify_tx_pkt(pkt, &tx_pkt);

	ret = ena_netdev_tx_xmit_one(ring, &tx_pkt,
				     used_bounce ? txq->bounce_buf :
						     (const void *)pkt->data,
				     &req_id);
	if (ret == 0) {
		if (used_bounce) {
			txq->bounce_in_use = true;
			txq->bounce_req_id = req_id;
			txq->bounce_wait_polls = 0;
		}
	}

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
	uint16_t q;

	if (!netdev)
		return;

	/* The stop operation drains the TX queues and destroys the
	 * hardware queues. It is a no-op when the device is not
	 * running. */
	ena_netdev_stop(netdev);

	for (q = 0; q < ENA_NETDEV_MAX_QUEUES; q++) {
		ena_netdev_free_rxq_bounce(&netdev->rx_queues[q]);
		ena_netdev_free_txq_bounce(&netdev->tx_queues[q]);
	}

	if (netdev->adapter) {
		ena_netdev_cleanup_adapter_rings(netdev->adapter);

		ena_intr_msix_fini(netdev->adapter);

		/* Destroy the AQ, ACQ and AENQ and release the host
		 * info buffer. Safe to call twice. */
		ena_admin_fini(netdev->adapter);
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

bool ena_netdev_link_get(struct uk_netdev *dev)
{
	if (!dev || !dev->adapter)
		return false;

	return dev->adapter->link_up;
}

#endif /* !__Unikraft__ */
