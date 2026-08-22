/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_datapath.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define ENA_DATAPATH_MAX_POLLS 100

static int ena_is_power_of_two(uint16_t val)
{
	return (val >= 4) && ((val & (val - 1)) == 0);
}

int ena_ring_alloc(struct ena_adapter *adapter, uint16_t qid,
		   enum ena_ring_type ring_type, uint16_t sq_depth,
		   uint16_t cq_depth, struct ena_ring **out_ring)
{
	struct ena_ring *ring;
	size_t sq_elem_size;
	size_t cq_elem_size;
	size_t buf_elem_size;
	size_t i;

	if (!adapter || !out_ring)
		return -EINVAL;

	if (!ena_is_power_of_two(sq_depth) || !ena_is_power_of_two(cq_depth)) {
		ena_err("ring alloc: queue depth must be power of two (sq=%u cq=%u)",
			sq_depth, cq_depth);
		return -EINVAL;
	}

	if (ring_type == ENA_RING_TYPE_TX) {
		sq_elem_size = sizeof(struct ena_eth_io_tx_desc);
		cq_elem_size = sizeof(struct ena_eth_io_tx_cdesc);
		buf_elem_size = sizeof(struct ena_tx_buffer);
	} else if (ring_type == ENA_RING_TYPE_RX) {
		sq_elem_size = sizeof(struct ena_eth_io_rx_desc);
		cq_elem_size = sizeof(struct ena_eth_io_rx_cdesc_base);
		buf_elem_size = sizeof(struct ena_rx_buffer);
	} else {
		ena_err("ring alloc: invalid ring type %d", ring_type);
		return -EINVAL;
	}

	ring = calloc(1, sizeof(*ring));
	if (!ring) {
		ena_err("ring alloc: failed to allocate ring context");
		return -ENOMEM;
	}

	ring->adapter = adapter;
	ring->qid = qid;
	ring->ring_type = ring_type;
	ring->sq_depth = sq_depth;
	ring->cq_depth = cq_depth;
	ring->sq_phase = 1;
	ring->cq_phase = 1;
	ring->sq_tail = 0;
	ring->sq_head = 0;
	ring->cq_head = 0;

	/* Allocate page-aligned DMA memory for Submission Queue */
	ring->sq_virt = ena_dma_alloc((size_t)sq_depth * sq_elem_size,
				      &ring->sq_phys);
	if (!ring->sq_virt) {
		ena_err("ring alloc: SQ DMA alloc failed");
		free(ring);
		return -ENOMEM;
	}
	memset(ring->sq_virt, 0, (size_t)sq_depth * sq_elem_size);

	/* Allocate page-aligned DMA memory for Completion Queue */
	ring->cq_virt = ena_dma_alloc((size_t)cq_depth * cq_elem_size,
				      &ring->cq_phys);
	if (!ring->cq_virt) {
		ena_err("ring alloc: CQ DMA alloc failed");
		ena_dma_free(ring->sq_virt, ring->sq_phys);
		free(ring);
		return -ENOMEM;
	}
	memset(ring->cq_virt, 0, (size_t)cq_depth * cq_elem_size);

	/* Allocate free request ID pool FIFO */
	ring->free_req_ids = calloc(sq_depth, sizeof(uint16_t));
	if (!ring->free_req_ids) {
		ena_err("ring alloc: failed to allocate req_id array");
		ena_dma_free(ring->cq_virt, ring->cq_phys);
		ena_dma_free(ring->sq_virt, ring->sq_phys);
		free(ring);
		return -ENOMEM;
	}

	/* Populate free request IDs */
	for (i = 0; i < sq_depth; i++)
		ring->free_req_ids[i] = (uint16_t)i;
	ring->free_req_head = 0;
	ring->free_req_tail = 0;
	ring->free_req_count = sq_depth;

	/* Allocate packet tracking array */
	ring->buffers.raw_bufs = calloc(sq_depth, buf_elem_size);
	if (!ring->buffers.raw_bufs) {
		ena_err("ring alloc: failed to allocate tracking buffers");
		free(ring->free_req_ids);
		ena_dma_free(ring->cq_virt, ring->cq_phys);
		ena_dma_free(ring->sq_virt, ring->sq_phys);
		free(ring);
		return -ENOMEM;
	}

	*out_ring = ring;
	return 0;
}

void ena_ring_free(struct ena_ring *ring)
{
	if (!ring)
		return;

	if (ring->buffers.raw_bufs) {
		free(ring->buffers.raw_bufs);
		ring->buffers.raw_bufs = NULL;
	}

	if (ring->free_req_ids) {
		free(ring->free_req_ids);
		ring->free_req_ids = NULL;
	}

	if (ring->cq_virt) {
		ena_dma_free(ring->cq_virt, ring->cq_phys);
		ring->cq_virt = NULL;
		ring->cq_phys = 0;
	}

	if (ring->sq_virt) {
		ena_dma_free(ring->sq_virt, ring->sq_phys);
		ring->sq_virt = NULL;
		ring->sq_phys = 0;
	}

	free(ring);
}

int ena_ring_req_id_alloc(struct ena_ring *ring, uint16_t *out_req_id)
{
	uint16_t id;

	if (!ring || !out_req_id)
		return -EINVAL;

	if (ring->free_req_count == 0)
		return -EBUSY;

	id = ring->free_req_ids[ring->free_req_head];
	ring->free_req_head = (uint16_t)((ring->free_req_head + 1) & (ring->sq_depth - 1));
	ring->free_req_count--;

	*out_req_id = id;
	return 0;
}

int ena_ring_req_id_free(struct ena_ring *ring, uint16_t req_id)
{
	if (!ring || req_id >= ring->sq_depth)
		return -EINVAL;

	if (ring->free_req_count >= ring->sq_depth)
		return -EINVAL;

	ring->free_req_ids[ring->free_req_tail] = req_id;
	ring->free_req_tail = (uint16_t)((ring->free_req_tail + 1) & (ring->sq_depth - 1));
	ring->free_req_count++;

	if (ring->ring_type == ENA_RING_TYPE_TX)
		memset(&ring->buffers.tx_bufs[req_id], 0, sizeof(struct ena_tx_buffer));
	else
		memset(&ring->buffers.rx_bufs[req_id], 0, sizeof(struct ena_rx_buffer));

	return 0;
}

int ena_admin_create_cq(struct ena_adapter *adapter, uint16_t cq_depth,
			uint64_t cq_phys, uint32_t msix_vector,
			uint16_t *out_cq_idx, uint32_t *out_db_offset)
{
	struct ena_admin_aq_create_cq_cmd cmd;
	struct ena_admin_acq_create_cq_resp_desc resp;
	int ret;

	if (!adapter || !out_cq_idx || !out_db_offset)
		return -EINVAL;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cq_caps_2 = 4; /* 4 words = 16 bytes entry size */
	cmd.cq_depth = cq_depth;
	cmd.msix_vector = msix_vector;
	cmd.cq_ba.mem_addr_low = (uint32_t)cq_phys;
	cmd.cq_ba.mem_addr_high = (uint16_t)(cq_phys >> 32);

	ret = ena_admin_exec_cmd(adapter, ENA_ADMIN_CREATE_CQ,
				 &cmd.cq_caps_1,
				 sizeof(cmd) - sizeof(cmd.aq_common_descriptor),
				 &resp.cq_idx,
				 sizeof(resp) - sizeof(resp.acq_common_desc),
				 NULL, ENA_DATAPATH_MAX_POLLS);
	if (ret)
		return ret;

	*out_cq_idx = resp.cq_idx;
	*out_db_offset = resp.cq_head_db_register_offset;
	return 0;
}

int ena_admin_destroy_cq(struct ena_adapter *adapter, uint16_t cq_idx)
{
	struct ena_admin_aq_destroy_cq_cmd cmd;
	struct ena_admin_acq_destroy_cq_resp_desc resp;

	if (!adapter)
		return -EINVAL;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cq_idx = cq_idx;

	return ena_admin_exec_cmd(adapter, ENA_ADMIN_DESTROY_CQ,
				  &cmd.cq_idx,
				  sizeof(cmd) - sizeof(cmd.aq_common_descriptor),
				  &resp, 0, NULL, ENA_DATAPATH_MAX_POLLS);
}

int ena_admin_create_sq(struct ena_adapter *adapter, uint16_t sq_depth,
			uint64_t sq_phys, uint16_t cq_idx, uint8_t direction,
			uint16_t *out_sq_idx, uint32_t *out_db_offset)
{
	struct ena_admin_aq_create_sq_cmd cmd;
	struct ena_admin_acq_create_sq_resp_desc resp;
	int ret;

	if (!adapter || !out_sq_idx || !out_db_offset)
		return -EINVAL;

	memset(&cmd, 0, sizeof(cmd));
	cmd.sq_identity = (uint8_t)((direction & 0x07u) << 5);
	cmd.sq_caps_2 = (uint8_t)(ENA_ADMIN_PLACEMENT_POLICY_HOST |
				  (ENA_ADMIN_COMPLETION_POLICY_CQE << 4));
	cmd.sq_caps_3 = 1; /* physically contiguous */
	cmd.cq_idx = cq_idx;
	cmd.sq_depth = sq_depth;
	cmd.sq_ba.mem_addr_low = (uint32_t)sq_phys;
	cmd.sq_ba.mem_addr_high = (uint16_t)(sq_phys >> 32);

	ret = ena_admin_exec_cmd(adapter, ENA_ADMIN_CREATE_SQ,
				 &cmd.sq_identity,
				 sizeof(cmd) - sizeof(cmd.aq_common_descriptor),
				 &resp.sq_idx,
				 sizeof(resp) - sizeof(resp.acq_common_desc),
				 NULL, ENA_DATAPATH_MAX_POLLS);
	if (ret)
		return ret;

	*out_sq_idx = resp.sq_idx;
	*out_db_offset = resp.sq_doorbell_offset;
	return 0;
}

int ena_admin_destroy_sq(struct ena_adapter *adapter, uint16_t sq_idx)
{
	struct ena_admin_aq_destroy_sq_cmd cmd;
	struct ena_admin_acq_destroy_sq_resp_desc resp;

	if (!adapter)
		return -EINVAL;

	memset(&cmd, 0, sizeof(cmd));
	cmd.sq.sq_idx = sq_idx;

	return ena_admin_exec_cmd(adapter, ENA_ADMIN_DESTROY_SQ,
				  &cmd.sq,
				  sizeof(cmd) - sizeof(cmd.aq_common_descriptor),
				  &resp, 0, NULL, ENA_DATAPATH_MAX_POLLS);
}

int ena_ring_create_hw(struct ena_ring *ring, uint32_t msix_vector)
{
	uint8_t direction;
	int ret;

	if (!ring || !ring->adapter || !ring->adapter->bar0_base)
		return -EINVAL;

	direction = (ring->ring_type == ENA_RING_TYPE_TX) ?
		    ENA_ADMIN_SQ_DIRECTION_TX : ENA_ADMIN_SQ_DIRECTION_RX;

	/* 1. Create Completion Queue */
	ret = ena_admin_create_cq(ring->adapter, ring->cq_depth, ring->cq_phys,
				  msix_vector, &ring->cq_idx, &ring->cq_db_offset);
	if (ret) {
		ena_err("ring create hw: failed to create CQ (%d)", ret);
		return ret;
	}

	if (ring->cq_db_offset != 0) {
		ring->cq_db = (volatile uint32_t *)
			(ring->adapter->bar0_base + ring->cq_db_offset);
	}

	/* 2. Create Submission Queue associated with CQ */
	ret = ena_admin_create_sq(ring->adapter, ring->sq_depth, ring->sq_phys,
				  ring->cq_idx, direction, &ring->sq_idx,
				  &ring->sq_db_offset);
	if (ret) {
		ena_err("ring create hw: failed to create SQ (%d)", ret);
		ena_admin_destroy_cq(ring->adapter, ring->cq_idx);
		return ret;
	}

	if (ring->sq_db_offset != 0) {
		ring->sq_db = (volatile uint32_t *)
			(ring->adapter->bar0_base + ring->sq_db_offset);
	}

	return 0;
}

int ena_ring_destroy_hw(struct ena_ring *ring)
{
	int ret = 0;
	int sq_ret;
	int cq_ret;

	if (!ring || !ring->adapter)
		return -EINVAL;

	sq_ret = ena_admin_destroy_sq(ring->adapter, ring->sq_idx);
	if (sq_ret) {
		ena_err("ring destroy hw: destroy SQ failed (%d)", sq_ret);
		ret = sq_ret;
	}

	cq_ret = ena_admin_destroy_cq(ring->adapter, ring->cq_idx);
	if (cq_ret) {
		ena_err("ring destroy hw: destroy CQ failed (%d)", cq_ret);
		if (!ret)
			ret = cq_ret;
	}

	ring->sq_db = NULL;
	ring->cq_db = NULL;

	return ret;
}
