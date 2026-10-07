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

	/* Refuse to submit to a ring whose hardware was destroyed by a
	 * reset. The netdev start path recreates the queue and restores
	 * validity before transmit may resume. */
	if (!ring->hw_valid)
		return -ENODEV;

	if (pkt->len == 0 || pkt->len > 0xFFFFu)
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

	/* Save packet metadata into tracking buffer */
	tx_buf = &ring->buffers.tx_bufs[req_id];
	tx_buf->netbuf = pkt->netbuf;
	tx_buf->phys_addr = pkt->phys_addr;
	tx_buf->data_len = pkt->len;
	tx_buf->num_descs = 1;
	tx_buf->req_id = req_id;

	/* Format TX submission descriptor */
	desc_ring = (struct ena_eth_io_tx_desc *)ring->sq_virt;
	desc = &desc_ring[ring->sq_tail & (ring->sq_depth - 1)];
	memset(desc, 0, sizeof(*desc));

	/* Word 0: len_ctrl */
	len_ctrl = (pkt->len & ENA_ETH_IO_TX_DESC_LENGTH_MASK);
	len_ctrl |= (((uint32_t)(req_id >> 10) & 0x3Fu)
		     << ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT);
	if (ring->sq_phase)
		len_ctrl |= ENA_ETH_IO_TX_DESC_PHASE_MASK;
	len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK |
		    ENA_ETH_IO_TX_DESC_LAST_MASK |
		    ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
	desc->len_ctrl = ena_cpu_to_le32(len_ctrl);

	/* Word 1: meta_ctrl */
	meta_ctrl = (((uint32_t)req_id & 0x03FFu)
		     << ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT);
	meta_ctrl |= (pkt->l3_proto & ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK);
	meta_ctrl |= (((uint32_t)pkt->l4_proto & 0x1Fu)
		      << ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT);
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
	desc->buff_addr_hi_hdr_sz =
	    ena_cpu_to_le32((uint32_t)((pkt->phys_addr >> 32) & 0xFFFFu));

	/* Advance producer tail index (monotonic unmasked counter) */
	ring->sq_tail++;
	if ((ring->sq_tail & (ring->sq_depth - 1)) == 0)
		ring->sq_phase ^= 1;

	ring->tx_packets++;
	ring->tx_bytes += pkt->len;

	/* Remember the core that owns this ring. The completion path
	 * frees the netbuf with the allocator stored in it, so a
	 * completion reaped on another core writes a foreign heap.
	 * [Ticket f47bdd0ed1] */
	ring->tx_owner_cpu = ena_plat_cpu_id();

	if (out_req_id)
		*out_req_id = req_id;

	ena_ring_unlock(ring);
	return 0;
}

void ena_tx_doorbell(struct ena_ring *ring)
{
	if (!ring || !ring->hw_valid || !ring->sq_db)
		return;

	ena_wmb();
	ena_reg_write32(ring->sq_db, ring->sq_tail);
	ena_mb();
}

/* Report a TX ring whose completions are reaped on a core other than
 * the one that transmits on it. The caller keeps this off the hot path:
 * it runs only after a CPU compare failed. One line per ring.
 * [Ticket f47bdd0ed1] */
static void ena_tx_report_foreign_reaper(struct ena_ring *ring, uint32_t cpu)
{
	if (ring->tx_owner_cpu == ENA_CPU_ID_NONE)
		return;
	if (ring->tx_owner_warned)
		return;

	ring->tx_owner_warned = true;
	ena_warn("tx q%u: completion reaped on cpu %u, last transmit on cpu %u",
		 (unsigned int)ring->qid, (unsigned int)cpu,
		 (unsigned int)ring->tx_owner_cpu);
}

/* Hand a completed buffer to the core that allocated it. The caller
 * runs on another core, and a per-core heap has no lock, so it must
 * not free the netbuf here. The request ID stays out of the free pool
 * until the owner frees the buffer. That keeps one slot in the
 * deferred array for every deferred entry. The caller holds the ring
 * lock. Returns true when the buffer is queued for its owner.
 * [Ticket b08b6c84c1] */
static bool ena_tx_stage_foreign_free(struct ena_ring *ring, uint16_t req_id,
				      uint32_t owner_cpu)
{
	struct ena_tx_buffer *tx_buf;
	struct ena_tx_foreign_free *slot;

	if (!ring->buffers.tx_bufs || !ring->tx_foreign)
		return false;

	tx_buf = &ring->buffers.tx_bufs[req_id];
	if (!tx_buf->netbuf)
		return false;

	if (ring->tx_foreign_count >= ring->sq_depth) {
		/* No room. Drop the buffer rather than free it in a
		 * foreign heap. */
		ena_err("tx q%u: no room to defer a buffer release",
			(unsigned int)ring->qid);
		tx_buf->netbuf = NULL;
		return false;
	}

	slot = &ring->tx_foreign[ring->tx_foreign_count++];
	slot->req_id = req_id;
	slot->owner_cpu = owner_cpu;
	return true;
}

/* Release the buffers that a foreign core completed on an earlier poll.
 * Only the core that allocated a netbuf may free it, so an entry that
 * belongs to another core stays in the array. The caller holds the
 * ring lock. [Ticket b08b6c84c1] */
static void ena_tx_drain_foreign_frees(struct ena_ring *ring, uint32_t cpu)
{
	struct ena_tx_buffer *tx_buf;
	uint16_t i;

	if (!ring->tx_foreign || ring->tx_foreign_count == 0)
		return;

	for (i = 0; i < ring->tx_foreign_count;) {
		struct ena_tx_foreign_free *slot = &ring->tx_foreign[i];

		if (slot->owner_cpu != cpu) {
			i++;
			continue;
		}

		tx_buf = &ring->buffers.tx_bufs[slot->req_id];
#ifdef __Unikraft__
		if (tx_buf->netbuf)
			uk_netbuf_free((struct uk_netbuf *)tx_buf->netbuf);
#endif
		tx_buf->netbuf = NULL;

		/* This returns the request ID to the free pool and clears
		 * the tracking entry. */
		ena_ring_req_id_free(ring, slot->req_id);

		/* Drop the entry: the last one takes its place. */
		*slot = ring->tx_foreign[--ring->tx_foreign_count];
	}
}

int ena_tx_poll_completions(struct ena_ring *ring, unsigned int budget,
			    unsigned int *cleaned_count)
{
	const struct ena_eth_io_tx_cdesc *cdesc_ring;
	const struct ena_eth_io_tx_cdesc *cdesc;
	unsigned int cleaned = 0;
	uint32_t cpu = ena_plat_cpu_id();
	uint16_t req_id;

	if (!ring || ring->ring_type != ENA_RING_TYPE_TX || !ring->cq_virt)
		return -EINVAL;

	/* After a reset the CQ memory may hold stale entries and the
	 * indices are fresh. Do not consume them until the queue is
	 * re-created. */
	if (!ring->hw_valid)
		return 0;

	/* No request is outstanding, so the device has no completion to
	 * post. Skip the lock and the CQ read. A completion can only exist
	 * for a submitted request that still holds a request ID, and that
	 * keeps free_req_count below sq_depth. This removes the per-poll
	 * and per-send lock cycle on an idle TX ring. [Ticket 292e049bf4] */
	if (ring->free_req_count >= ring->sq_depth) {
		if (cleaned_count)
			*cleaned_count = 0;
		return 0;
	}

	if (budget == 0)
		budget = ring->cq_depth;

	ena_ring_lock(ring);

	/* Release the buffers that a foreign core completed on an earlier
	 * poll. A deferred entry keeps its request ID out of the free
	 * pool, so the idle check above never skips this drain.
	 * [Ticket b08b6c84c1] */
	ena_tx_drain_foreign_frees(ring, cpu);

	cdesc_ring = (const struct ena_eth_io_tx_cdesc *)ring->cq_virt;

	while (cleaned < budget) {
		volatile const uint8_t *flags_ptr;

		flags_ptr =
		    (volatile const uint8_t *)&cdesc_ring[ring->cq_head &
							  (ring->cq_depth - 1)]
			.flags;
		if ((*flags_ptr & ENA_ETH_IO_TX_CDESC_PHASE_MASK) !=
		    ring->cq_phase)
			break;

		ena_rmb();
		cdesc = &cdesc_ring[ring->cq_head & (ring->cq_depth - 1)];

		req_id = ena_le16_to_cpu(cdesc->req_id);
		if (req_id >= ring->sq_depth) {
			ena_err("tx poll: invalid req_id %u from device",
				req_id);
			break;
		}

		/* Validate in-flight request status. The device may complete
		 * out of order, so a completion is matched by its own req_id.
		 * A req_id is only ever returned to the free pool here, after
		 * its completion, so it is never reused while outstanding.
		 * [Ticket a9c6945c21] */
		if (!ring->req_in_flight || !ring->req_in_flight[req_id]) {
			ena_err("tx poll: req_id %u not in-flight", req_id);
			ring->cq_head++;
			if ((ring->cq_head & (ring->cq_depth - 1)) == 0)
				ring->cq_phase ^= 1;
			continue;
		}

		ring->req_in_flight[req_id] = 0;

		/* Reclaim this request's resources at completion time. The
		 * netdev layer returns the matching bounce slot through the
		 * callback, so it never scans the whole map. [Ticket
		 * 292e049bf4] */
		if (ring->tx_complete_cb)
			ring->tx_complete_cb(ring->tx_complete_arg, req_id);

		/* Update SQ head index acknowledged by controller */
		ring->sq_head =
		    ena_le16_to_cpu(cdesc->sq_head_idx) & (ring->sq_depth - 1);

		/* One compare per completion. A mismatch means the free
		 * below runs in another core's heap, so hand the buffer
		 * to the core that allocated it instead. [Ticket
		 * f47bdd0ed1] [Ticket b08b6c84c1] */
		if (cpu != ring->tx_owner_cpu) {
			ena_tx_report_foreign_reaper(ring, cpu);
			if (ena_tx_stage_foreign_free(ring, req_id,
						      ring->tx_owner_cpu))
				goto advance_cq;
		}

		/* Reclaim transmitted packet buffer */
		if (ring->buffers.tx_bufs) {
			struct ena_tx_buffer *tx_buf =
			    &ring->buffers.tx_bufs[req_id];
#ifdef __Unikraft__
			if (tx_buf->netbuf)
				uk_netbuf_free(
				    (struct uk_netbuf *)tx_buf->netbuf);
#endif
			tx_buf->netbuf = NULL;
		}

		/* Return request ID to free pool */
		ena_ring_req_id_free(ring, req_id);

advance_cq:
		/* Advance CQ consumer head index (monotonic unmasked counter)
		 */
		ring->cq_head++;
		if ((ring->cq_head & (ring->cq_depth - 1)) == 0)
			ring->cq_phase ^= 1;

		cleaned++;
	}

	ena_ring_unlock(ring);

	if (cleaned_count)
		*cleaned_count = cleaned;

	return (int)cleaned;
}
