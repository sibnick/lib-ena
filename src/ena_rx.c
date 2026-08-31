/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_datapath.h"

#ifdef __Unikraft__
#include <uk/netbuf.h>
#endif

#include <errno.h>
#include <string.h>

uint16_t ena_rx_free_space(const struct ena_ring *ring)
{
	if (!ring || ring->ring_type != ENA_RING_TYPE_RX)
		return 0;

	return ring->free_req_count;
}

int ena_rx_submit_one(struct ena_ring *ring, void *netbuf, uint64_t phys_addr,
		      uint32_t buf_len, uint16_t *out_req_id)
{
	struct ena_eth_io_rx_desc *desc_ring;
	struct ena_eth_io_rx_desc *desc;
	struct ena_rx_buffer *rx_buf;
	uint16_t req_id;
	uint8_t ctrl;
	int ret;

	if (!ring || !netbuf || ring->ring_type != ENA_RING_TYPE_RX)
		return -EINVAL;

	if (buf_len == 0 || buf_len > 0xFFFFu)
		return -EINVAL;

	ena_ring_lock(ring);

	if (ring->free_req_count == 0) {
		ena_ring_unlock(ring);
		return -EBUSY;
	}

	ret = ena_ring_req_id_alloc(ring, &req_id);
	if (ret) {
		ena_ring_unlock(ring);
		return ret;
	}

	/* Mark request as in-flight */
	if (ring->req_in_flight)
		ring->req_in_flight[req_id] = 1;

	/* Save receive buffer tracking metadata */
	rx_buf = &ring->buffers.rx_bufs[req_id];
	rx_buf->netbuf = netbuf;
	rx_buf->phys_addr = phys_addr;
	rx_buf->data_len = buf_len;
	rx_buf->req_id = req_id;

	/* Format RX Submission Queue descriptor */
	desc_ring = (struct ena_eth_io_rx_desc *)ring->sq_virt;
	desc = &desc_ring[ring->sq_tail & (ring->sq_depth - 1)];
	memset(desc, 0, sizeof(*desc));

	desc->length = ena_cpu_to_le16((uint16_t)buf_len);
	ctrl = (ring->sq_phase & ENA_ETH_IO_RX_DESC_PHASE_MASK);
	ctrl |= ENA_ETH_IO_RX_DESC_FIRST_MASK |
		ENA_ETH_IO_RX_DESC_LAST_MASK |
		ENA_ETH_IO_RX_DESC_COMP_REQ_MASK;
	desc->ctrl = ctrl;
	desc->req_id = ena_cpu_to_le16(req_id);
	desc->buff_addr_lo = ena_cpu_to_le32((uint32_t)phys_addr);
	desc->buff_addr_hi = ena_cpu_to_le16((uint16_t)(phys_addr >> 32));

	/* Advance producer tail index (monotonic unmasked counter) */
	ring->sq_tail++;
	if ((ring->sq_tail & (ring->sq_depth - 1)) == 0)
		ring->sq_phase ^= 1;

	if (out_req_id)
		*out_req_id = req_id;

	ena_ring_unlock(ring);
	return 0;
}

int ena_rx_refill(struct ena_ring *ring, unsigned int count,
		  void *(*alloc_netbuf)(void *arg, uint64_t *phys_out, uint32_t *len_out),
		  void *alloc_arg, unsigned int *refilled_count)
{
	unsigned int refilled = 0;
	uint64_t phys;
	uint32_t len;
	void *nb;
	int ret;

	if (!ring || !alloc_netbuf || ring->ring_type != ENA_RING_TYPE_RX)
		return -EINVAL;

	if (count == 0)
		count = ring->free_req_count;

	while (refilled < count && ring->free_req_count > 0) {
		phys = 0;
		len = 0;
		nb = alloc_netbuf(alloc_arg, &phys, &len);
		if (!nb)
			break;

		ret = ena_rx_submit_one(ring, nb, phys, len, NULL);
		if (ret)
			break;

		refilled++;
	}

	if (refilled > 0)
		ena_rx_doorbell(ring);

	if (refilled_count)
		*refilled_count = refilled;

	return (int)refilled;
}

void ena_rx_doorbell(struct ena_ring *ring)
{
	if (!ring || !ring->sq_db)
		return;

	ena_wmb();
	ena_reg_write32(ring->sq_db, ring->sq_tail);
	ena_mb();
}

int ena_rx_poll(struct ena_ring *ring, struct ena_rx_pkt *pkts,
		unsigned int max_pkts)
{
	const struct ena_eth_io_rx_cdesc_base *cdesc_ring;
	const struct ena_eth_io_rx_cdesc_base *cdesc;
	struct ena_rx_buffer *rx_buf;
	unsigned int rcvd = 0;
	uint16_t req_id;
	uint16_t pkt_len;
	uint8_t phase;

	if (!ring || !pkts || ring->ring_type != ENA_RING_TYPE_RX || !ring->cq_virt || max_pkts == 0)
		return -EINVAL;

	ena_ring_lock(ring);

	cdesc_ring = (const struct ena_eth_io_rx_cdesc_base *)ring->cq_virt;

	while (rcvd < max_pkts) {
		volatile const uint32_t *status_ptr;
		uint32_t status_val;

		cdesc = &cdesc_ring[ring->cq_head & (ring->cq_depth - 1)];
		status_ptr = (volatile const uint32_t *)&cdesc->status;
		status_val = ena_le32_to_cpu(*status_ptr);

		phase = (uint8_t)((status_val & ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK) >>
				  ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT);
		if (phase != ring->cq_phase)
			break;

		ena_rmb();

		req_id = ena_le16_to_cpu(cdesc->req_id);
		if (req_id >= ring->sq_depth) {
			ena_err("rx poll: invalid req_id %u from device", req_id);
			break;
		}

		/* Validate in-flight request status */
		if (!ring->req_in_flight || !ring->req_in_flight[req_id]) {
			ena_err("rx poll: req_id %u not in-flight", req_id);
			ring->cq_head++;
			if ((ring->cq_head & (ring->cq_depth - 1)) == 0)
				ring->cq_phase ^= 1;
			continue;
		}

		rx_buf = &ring->buffers.rx_bufs[req_id];
		pkt_len = ena_le16_to_cpu(cdesc->length);

		/* Validate packet length against buffer capacity */
		if (pkt_len > rx_buf->data_len) {
			ena_err("rx poll: packet length %u exceeds buffer capacity %u",
				pkt_len, rx_buf->data_len);
			ring->req_in_flight[req_id] = 0;
#ifdef __Unikraft__
			if (rx_buf->netbuf)
				uk_netbuf_free((struct uk_netbuf *)rx_buf->netbuf);
#endif
			rx_buf->netbuf = NULL;
			ena_ring_req_id_free(ring, req_id);
			ring->cq_head++;
			if ((ring->cq_head & (ring->cq_depth - 1)) == 0)
				ring->cq_phase ^= 1;
			continue;
		}

		ring->req_in_flight[req_id] = 0;

		pkts[rcvd].netbuf = rx_buf->netbuf;
		pkts[rcvd].len = pkt_len;
		pkts[rcvd].hash = ena_le32_to_cpu(cdesc->hash);
		pkts[rcvd].req_id = req_id;
		pkts[rcvd].l3_csum_err = !!(status_val & ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK);
		pkts[rcvd].l4_csum_err = !!(status_val & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK);
		pkts[rcvd].l4_csum_checked = !!(status_val & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK);
		pkts[rcvd].frag = !!(status_val & ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK);

		ring->rx_packets++;
		ring->rx_bytes += pkts[rcvd].len;

		/* Return request ID back to free pool */
		ena_ring_req_id_free(ring, req_id);

		/* Advance CQ consumer head index (monotonic unmasked counter) */
		ring->cq_head++;
		if ((ring->cq_head & (ring->cq_depth - 1)) == 0)
			ring->cq_phase ^= 1;

		rcvd++;
	}

	if (rcvd > 0 && ring->cq_db)
		ena_reg_write32(ring->cq_db, ring->cq_head);

	ena_ring_unlock(ring);

	return (int)rcvd;
}
