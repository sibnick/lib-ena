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
	struct {
		struct ena_admin_ctrl_buff_info control_buffer;
		struct {
			uint8_t flags;
			uint8_t feature_id;
			uint8_t feature_version;
			uint8_t reserved8;
		} feat_common;
		uint32_t raw[11];
	} req;
	struct ena_admin_feature_llq_desc resp;
	uint16_t cmd_id = 0;
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&adapter->llq_info, 0, sizeof(adapter->llq_info));

	/* Check if BAR2 is mapped */
	if (!adapter->bar2_base || adapter->bar2_size == 0) {
		adapter->llq_info.supported = false;
		adapter->llq_info.enabled = false;
		return 0;
	}

	memset(&req, 0, sizeof(req));
	req.feat_common.feature_id = ENA_ADMIN_LLQ;
	req.feat_common.flags = 0x1; /* Current value */

	memset(&resp, 0, sizeof(resp));
	ret = ena_admin_exec_cmd(adapter, ENA_ADMIN_GET_FEATURE, &req, sizeof(req),
				 &resp, sizeof(resp), &cmd_id, 100);
	if (ret) {
		adapter->llq_info.supported = false;
		adapter->llq_info.enabled = false;
		return 0;
	}

	adapter->llq_info.supported = true;
	adapter->llq_info.enabled = true;
	adapter->llq_info.max_llq_num = resp.max_llq_num;
	adapter->llq_info.max_llq_depth = resp.max_llq_depth;
	adapter->llq_info.entry_size = 128;
	adapter->llq_info.header_len = 96;

	return 0;
}

int ena_llq_tx_push(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		    const void *hdr_data, uint16_t hdr_len,
		    uint16_t *out_req_id)
{
	struct ena_eth_io_tx_desc desc;
	struct ena_tx_buffer *tx_buf;
	uint8_t *push_dest;
	uint16_t req_id;
	uint32_t len_ctrl;
	uint32_t meta_ctrl;
	uint32_t buff_hi_hdr;
	int ret;

	if (!ring || !pkt || ring->ring_type != ENA_RING_TYPE_TX)
		return -EINVAL;

	/* Fall back to standard host-memory submission if LLQ is not enabled */
	if (!ring->is_llq || !ring->push_buf_virt) {
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

	if (hdr_len > 96)
		return -EINVAL;

	if (ring->free_req_count == 0)
		return -EBUSY;

	ret = ena_ring_req_id_alloc(ring, &req_id);
	if (ret)
		return ret;

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

	/* Build aligned 128-byte cache-line entry (desc + header + zero pad) */
	uint8_t entry_buf[128];
	memset(entry_buf, 0, sizeof(entry_buf));
	memcpy(entry_buf, &desc, sizeof(desc));
	if (hdr_data && hdr_len > 0)
		memcpy(entry_buf + sizeof(desc), hdr_data, hdr_len);

	push_dest = (uint8_t *)ring->push_buf_virt + ((size_t)ring->sq_tail * 128);
	memcpy(push_dest, entry_buf, sizeof(entry_buf));

	/* Advance SQ producer tail */
	ring->sq_tail = (uint16_t)((ring->sq_tail + 1) & (ring->sq_depth - 1));
	if (ring->sq_tail == 0)
		ring->sq_phase ^= 1;

	ring->tx_packets++;
	ring->tx_bytes += pkt->len;

	/* Ring MMIO doorbell */
	ena_tx_doorbell(ring);

	if (out_req_id)
		*out_req_id = req_id;

	return 0;
}
