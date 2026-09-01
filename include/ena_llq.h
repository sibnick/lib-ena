/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_LLQ_H
#define LIBENA_ENA_LLQ_H

#include <stdint.h>
#include <stdbool.h>

#include "ena_init.h"

struct ena_adapter;
struct ena_ring;
struct ena_tx_pkt;

enum ena_llq_header_location {
	ENA_LLQ_INLINE_HEADER = 1,
	ENA_LLQ_HEADER_RING   = 2,
};

enum ena_llq_entry_size {
	ENA_LLQ_ENTRY_SIZE_128B = 1,
	ENA_LLQ_ENTRY_SIZE_192B = 2,
	ENA_LLQ_ENTRY_SIZE_256B = 4,
};

/* Stride control for LLQ descriptor entries (reference/ena_admin_defs.h). */
enum ena_llq_stride_ctrl {
	ENA_LLQ_SINGLE_DESC_PER_ENTRY     = 1,
	ENA_LLQ_MULTIPLE_DESCS_PER_ENTRY  = 2,
};

/* Wire format for LLQ Get/Set Feature */
struct ena_admin_feature_llq_desc {
	uint32_t max_llq_num;
	uint32_t max_llq_depth;
	uint16_t header_location_ctrl_supported;
	uint16_t header_location_ctrl_enabled;
	uint16_t entry_size_ctrl_supported;
	uint16_t entry_size_ctrl_enabled;
	uint16_t desc_num_before_header_supported;
	uint16_t desc_num_before_header_enabled;
	uint16_t descriptors_stride_ctrl_supported;
	uint16_t descriptors_stride_ctrl_enabled;
	uint32_t reserved1;
	uint32_t reserved2;
};

/* LLQ Configuration Context */
struct ena_llq_info {
	bool supported;
	bool enabled;
	uint32_t max_llq_num;
	uint32_t max_llq_depth;
	uint16_t header_len;
	uint16_t entry_size;
};

/**
 * Select the LLQ entry layout from a device LLQ feature descriptor.
 * The driver uses inline headers in a 128-byte entry. That layout holds
 * one 16-byte descriptor and a 96-byte header, which covers standard
 * L2 to L4 headers.
 *
 * @param desc Pointer to the LLQ feature descriptor from the device.
 * @param entry_size_out Output pointer for the entry size in bytes.
 * @param header_len_out Output pointer for the inline header capacity in bytes.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_llq_select_params(const struct ena_admin_feature_llq_desc *desc,
			  uint16_t *entry_size_out, uint16_t *header_len_out);

/**
 * Negotiate Low Latency Queue (LLQ) features and entry size with the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_llq_negotiate(struct ena_adapter *adapter);

/**
 * Push a transmit packet descriptor and inline header directly to BAR2 MMIO.
 *
 * @param ring Pointer to the active TX ring structure.
 * @param pkt Pointer to the transmit packet metadata and payload info.
 * @param hdr_data Pointer to the inline packet header bytes.
 * @param hdr_len Length of the inline packet header in bytes.
 * @param out_req_id Output pointer for the assigned request ID.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_llq_tx_push(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		    const void *hdr_data, uint16_t hdr_len,
		    uint16_t *out_req_id);

#endif /* LIBENA_ENA_LLQ_H */

