/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_llq.h"
#include "ena_admin.h"
#include "ena_datapath.h"
#include "ena_netdev.h"

#include <errno.h>
#include <string.h>

int ena_llq_negotiate(struct ena_adapter *adapter)
{
	struct ena_admin_get_feat_inline req;
	struct ena_admin_feature_llq_desc resp;
	uint16_t entry_size = 0;
	uint16_t header_len = 0;
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&adapter->llq_info, 0, sizeof(adapter->llq_info));

	/* LLQ needs the BAR2 MMIO region. Without it the device stays in
	 * standard host-memory mode. */
	if (!adapter->bar2_base || adapter->bar2_size == 0)
		return 0;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_LLQ;

	memset(&resp, 0, sizeof(resp));
	ret = ena_admin_exec_cmd(adapter, ENA_ADMIN_GET_FEATURE, &req, sizeof(req),
				 &resp, sizeof(resp), NULL, 100);
	if (ret)
		return 0;

	ret = ena_llq_select_params(&resp, &entry_size, &header_len);
	if (ret)
		return 0;

	adapter->llq_info.supported = true;
	adapter->llq_info.enabled = true;
	adapter->llq_info.max_llq_num = resp.max_llq_num;
	adapter->llq_info.max_llq_depth = resp.max_llq_depth;
	adapter->llq_info.entry_size = entry_size;
	adapter->llq_info.header_len = header_len;

	return 0;
}

int ena_llq_tx_push(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		    const void *hdr_data, uint16_t hdr_len,
		    uint16_t *out_req_id)
{
	struct ena_eth_io_tx_desc desc;
	struct ena_tx_buffer *tx_buf;
	uint8_t entry_buf[256] __attribute__((aligned(8)));
	uint8_t *push_dest;
	uint32_t entry_size;
	uint32_t header_cap;
	uint16_t req_id;
	uint32_t len_ctrl;
	uint32_t meta_ctrl;
	uint32_t buff_hi_hdr;
	size_t slot_idx;
	size_t i;
	int ret;

	if (!ring || !pkt || ring->ring_type != ENA_RING_TYPE_TX)
		return -EINVAL;

	entry_size = ring->llq_entry_size ? ring->llq_entry_size : 128;
	header_cap = (uint32_t)(entry_size -
				2 * (uint32_t)sizeof(struct ena_eth_io_tx_desc));

	if (hdr_len > header_cap)
		return -EINVAL;

	/* Fall back to standard host-memory submission if LLQ is not enabled or push buffer is too small */
	if (!ring->is_llq || !ring->push_buf_virt ||
	    (ring->push_buf_size > 0 && (size_t)ring->sq_depth * entry_size > ring->push_buf_size)) {
		struct ena_tx_pkt fallback_pkt = *pkt;
		if (hdr_data && hdr_len > 0 && fallback_pkt.netbuf) {
			struct uk_netbuf *nb = (struct uk_netbuf *)fallback_pkt.netbuf;
			if (nb->data)
				memcpy(nb->data, hdr_data, hdr_len);
		}
		ret = ena_tx_submit(ring, &fallback_pkt, out_req_id);
		if (ret == 0)
			ena_tx_doorbell(ring);
		return ret;
	}

	ena_ring_lock(ring);

	if (ring->free_req_count == 0) {
		ena_ring_unlock(ring);
		return -EBUSY;
	}

	slot_idx = (size_t)(ring->sq_tail & (ring->sq_depth - 1));

	/* Keep the MMIO write inside the push buffer when a size is known */
	if (ring->push_buf_size > 0 &&
	    (slot_idx + 1) * (size_t)entry_size > ring->push_buf_size) {
		ena_ring_unlock(ring);
		return -EINVAL;
	}

	ret = ena_ring_req_id_alloc(ring, &req_id);
	if (ret) {
		ena_ring_unlock(ring);
		return ret;
	}

	if (ring->req_in_flight)
		ring->req_in_flight[req_id] = 1;

	/* Save packet tracking metadata */
	tx_buf = &ring->buffers.tx_bufs[req_id];
	tx_buf->netbuf = pkt->netbuf;
	tx_buf->phys_addr = pkt->phys_addr;
	tx_buf->data_len = pkt->len;
	tx_buf->num_descs = 1;
	tx_buf->req_id = req_id;

	/* Build LLQ descriptor */
	memset(&desc, 0, sizeof(desc));

	len_ctrl = (pkt->len & ENA_ETH_IO_TX_DESC_LENGTH_MASK);
	len_ctrl |= (((uint32_t)(req_id >> 10) & 0x3Fu) <<
		     ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT);
	if (ring->sq_phase)
		len_ctrl |= ENA_ETH_IO_TX_DESC_PHASE_MASK;
	len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK |
		    ENA_ETH_IO_TX_DESC_LAST_MASK |
		    ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
	desc.len_ctrl = ena_cpu_to_le32(len_ctrl);

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
	desc.meta_ctrl = ena_cpu_to_le32(meta_ctrl);

	desc.buff_addr_lo = ena_cpu_to_le32((uint32_t)pkt->phys_addr);
	buff_hi_hdr = (uint32_t)((pkt->phys_addr >> 32) & 0xFFFFu);
	buff_hi_hdr |= ((uint32_t)hdr_len << 24);
	desc.buff_addr_hi_hdr_sz = ena_cpu_to_le32(buff_hi_hdr);

	/* Build the entry: descriptor, inline header, zero pad */
	memset(entry_buf, 0, sizeof(entry_buf));
	memcpy(entry_buf, &desc, sizeof(desc));
	if (hdr_data && hdr_len > 0)
		memcpy(entry_buf + sizeof(desc), hdr_data, hdr_len);

	push_dest = (uint8_t *)ring->push_buf_virt + slot_idx * (size_t)entry_size;

	/* Copy the entry to MMIO write-combining memory with 64-bit word writes */
	{
		volatile uint64_t *dst64 = (volatile uint64_t *)push_dest;
		const uint64_t *src64 = (const uint64_t *)entry_buf;

		for (i = 0; i < entry_size / sizeof(uint64_t); i++)
			dst64[i] = src64[i];
	}

	/* Advance producer tail index (monotonic unmasked counter) */
	ring->sq_tail++;
	if ((ring->sq_tail & (ring->sq_depth - 1)) == 0)
		ring->sq_phase ^= 1;

	ring->tx_packets++;
	ring->tx_bytes += pkt->len;

	/* Ring MMIO doorbell */
	ena_wmb();
	ena_tx_doorbell(ring);

	if (out_req_id)
		*out_req_id = req_id;

	ena_ring_unlock(ring);
	return 0;
}
