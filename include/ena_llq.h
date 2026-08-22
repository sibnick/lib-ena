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

/* Negotiate LLQ feature with the controller */
int ena_llq_negotiate(struct ena_adapter *adapter);

/* Transmit packet directly using LLQ push buffer */
int ena_llq_tx_push(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		    const void *hdr_data, uint16_t hdr_len,
		    uint16_t *out_req_id);

#endif /* LIBENA_ENA_LLQ_H */
