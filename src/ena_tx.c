/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_datapath.h"

#include <errno.h>
#include <string.h>

uint16_t ena_tx_free_space(const struct ena_ring *ring)
{
	if (!ring || ring->ring_type != ENA_RING_TYPE_TX)
		return 0;

	return ring->free_req_count;
}

int ena_tx_submit(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		  uint16_t *out_req_id)
{
	struct ena_eth_io_tx_desc *desc_ring;
	struct ena_eth_io_tx_desc *desc;
	struct ena_tx_buffer *tx_buf;
	uint16_t req_id;
	uint32_t len_ctrl;
	uint32_t meta_ctrl;
	int ret;

	if (!ring || !pkt || ring->ring_type != ENA_RING_TYPE_TX)
		return -EINVAL;

	if (pkt->len == 0 || pkt->len > 0xFFFFu)
		return -EINVAL;

	if (ring->free_req_count == 0)
		return -EBUSY;

	ret = ena_ring_req_id_alloc(ring, &req_id);
	if (ret)
		return ret;

	/* Save packet metadata into tracking buffer */
	tx_buf = &ring->buffers.tx_bufs[req_id];
	tx_buf->netbuf = pkt->netbuf;
	tx_buf->phys_addr = pkt->phys_addr;
	tx_buf->data_len = pkt->len;
	tx_buf->num_descs = 1;
	tx_buf->req_id = req_id;

	/* Format TX submission descriptor */
	desc_ring = (struct ena_eth_io_tx_desc *)ring->sq_virt;
	desc = &desc_ring[ring->sq_tail];
	memset(desc, 0, sizeof(*desc));

	/* Word 0: len_ctrl */
	len_ctrl = (pkt->len & ENA_ETH_IO_TX_DESC_LENGTH_MASK);
	len_ctrl |= (((uint32_t)(req_id >> 10) & 0x3Fu) <<
		     ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT);
	if (ring->sq_phase)
		len_ctrl |= ENA_ETH_IO_TX_DESC_PHASE_MASK;
	len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK |
		    ENA_ETH_IO_TX_DESC_LAST_MASK |
		    ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
	desc->len_ctrl = ena_cpu_to_le32(len_ctrl);

	/* Word 1: meta_ctrl */
	meta_ctrl = (((uint32_t)req_id & 0x03FFu) <<
		     ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT);
	meta_ctrl |= (pkt->l3_proto & ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK);
	meta_ctrl |= (((uint32_t)pkt->l4_proto & 0x1Fu) <<
		      ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT);
	if (pkt->l3_csum_en)
		meta_ctrl |= ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK;
	if (pkt->l4_csum_en)
		meta_ctrl |= ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK;
	if (pkt->df)
		meta_ctrl |= ENA_ETH_IO_TX_DESC_DF_MASK;
	if (pkt->tso_en)
		meta_ctrl |= ENA_ETH_IO_TX_DESC_TSO_EN_MASK;
	desc->meta_ctrl = ena_cpu_to_le32(meta_ctrl);

	/* Word 2 & 3: buffer physical address */
	desc->buff_addr_lo = ena_cpu_to_le32((uint32_t)pkt->phys_addr);
	desc->buff_addr_hi_hdr_sz = ena_cpu_to_le32((uint32_t)((pkt->phys_addr >> 32) & 0xFFFFu));

	/* Advance producer tail index */
	ring->sq_tail = (uint16_t)((ring->sq_tail + 1) & (ring->sq_depth - 1));
	if (ring->sq_tail == 0)
		ring->sq_phase ^= 1;

	ring->tx_packets++;
	ring->tx_bytes += pkt->len;

	if (out_req_id)
		*out_req_id = req_id;

	return 0;
}

void ena_tx_doorbell(struct ena_ring *ring)
{
	if (!ring || !ring->sq_db)
		return;

	ena_wmb();
	ena_reg_write32(ring->sq_db, ring->sq_tail);
	ena_mb();
}

int ena_tx_poll_completions(struct ena_ring *ring, unsigned int budget,
			    unsigned int *cleaned_count)
{
	const struct ena_eth_io_tx_cdesc *cdesc_ring;
	const struct ena_eth_io_tx_cdesc *cdesc;
	unsigned int cleaned = 0;
	uint16_t req_id;

	if (!ring || ring->ring_type != ENA_RING_TYPE_TX || !ring->cq_virt)
		return -EINVAL;

	if (budget == 0)
		budget = ring->cq_depth;

	cdesc_ring = (const struct ena_eth_io_tx_cdesc *)ring->cq_virt;

	while (cleaned < budget) {
		volatile const uint8_t *flags_ptr;

		flags_ptr = (volatile const uint8_t *)&cdesc_ring[ring->cq_head].flags;
		if ((*flags_ptr & ENA_ETH_IO_TX_CDESC_PHASE_MASK) != ring->cq_phase)
			break;

		ena_rmb();
		cdesc = &cdesc_ring[ring->cq_head];

		req_id = ena_le16_to_cpu(cdesc->req_id);
		if (req_id >= ring->sq_depth) {
			ena_err("tx poll: invalid req_id %u from device", req_id);
			break;
		}

		/* Update SQ head index acknowledged by controller */
		ring->sq_head = ena_le16_to_cpu(cdesc->sq_head_idx) & (ring->sq_depth - 1);

		/* Reclaim transmitted packet buffer */
		if (ring->buffers.tx_bufs) {
			struct ena_tx_buffer *tx_buf = &ring->buffers.tx_bufs[req_id];
#ifdef __Unikraft__
			if (tx_buf->netbuf)
				uk_netbuf_free((struct uk_netbuf *)tx_buf->netbuf);
#endif
			tx_buf->netbuf = NULL;
		}

		/* Return request ID to free pool */
		ena_ring_req_id_free(ring, req_id);

		/* Advance CQ consumer head index */
		ring->cq_head = (uint16_t)((ring->cq_head + 1) & (ring->cq_depth - 1));
		if (ring->cq_head == 0)
			ring->cq_phase ^= 1;

		cleaned++;
	}

	if (cleaned_count)
		*cleaned_count = cleaned;

	return (int)cleaned;
}
