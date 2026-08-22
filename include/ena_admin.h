/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_ADMIN_H
#define LIBENA_ENA_ADMIN_H

#include <stdint.h>
#include <stddef.h>

/* 48-bit physical memory address (low 32 bits + high 16 bits). */
struct ena_common_mem_addr {
	uint32_t mem_addr_low;
	uint16_t mem_addr_high;
	uint16_t reserved16;
};

/* Common descriptor shared by all admin queue entries. */
struct ena_admin_aq_common_desc {
	uint16_t command_id;   /* bits 11:0, upper 4 bits reserved */
	uint8_t opcode;
	uint8_t flags;         /* bit 0: phase */
};

/* Control buffer reference (used when flags bit 1 is set). */
struct ena_admin_ctrl_buff_info {
	uint32_t length;
	struct ena_common_mem_addr address;
};

/* Admin queue (request) entry. Total size: 64 bytes. */
struct ena_admin_aq_entry {
	struct ena_admin_aq_common_desc aq_common_desc;

	union {
		uint32_t inline_data_w1[3];
		struct ena_admin_ctrl_buff_info control_buffer;
	} u;

	uint32_t inline_data_w4[12];
};

/* Admin completion queue (response) common descriptor. */
struct ena_admin_acq_common_desc {
	uint16_t command;          /* matched command_id, bits 11:0 */
	uint8_t status;            /* 0 = success */
	uint8_t flags;             /* bit 0: phase */
	uint16_t extended_status;
	uint16_t sq_head_indx;
};

/* Admin completion queue entry. Total size: 64 bytes. */
struct ena_admin_acq_entry {
	struct ena_admin_acq_common_desc acq_common_desc;
	uint32_t response_specific_data[14];
};

/* Asynchronous event notification queue common descriptor. */
struct ena_admin_aenq_common_desc {
	uint16_t group;
	uint16_t syndrome;
	uint8_t flags;             /* bit 0: phase */
	uint8_t reserved1[3];
	uint32_t timestamp_low;
	uint32_t timestamp_high;
};

/* Asynchronous event notification queue entry. Total size: 64 bytes. */
struct ena_admin_aenq_entry {
	struct ena_admin_aenq_common_desc aenq_common_desc;
	uint32_t inline_data_w4[12];
};

/* Phase bit masks (all in the flags byte, bit 0). */
#define ENA_ADMIN_AQ_PHASE_MASK		0x01
#define ENA_ADMIN_ACQ_PHASE_MASK	0x01
#define ENA_ADMIN_AENQ_PHASE_MASK	0x01

/* Command id field width (bits 11:0). */
#define ENA_ADMIN_COMMAND_ID_MASK	0x0FFF

/* Inline data region of an AQ entry (64 bytes - 4 byte common desc). */
#define ENA_ADMIN_AQ_INLINE_DATA_SIZE	60

/* Admin queue opcodes. */
enum ena_admin_aq_opcode {
	ENA_ADMIN_CREATE_SQ	= 1,
	ENA_ADMIN_DESTROY_SQ	= 2,
	ENA_ADMIN_CREATE_CQ	= 3,
	ENA_ADMIN_DESTROY_CQ	= 4,
	ENA_ADMIN_GET_FEATURE	= 8,
	ENA_ADMIN_SET_FEATURE	= 9,
	ENA_ADMIN_GET_STATS	= 11,
};

/* Queue direction for CREATE_SQ */
enum ena_admin_sq_direction {
	ENA_ADMIN_SQ_DIRECTION_TX = 1,
	ENA_ADMIN_SQ_DIRECTION_RX = 2,
};

/* Queue placement policy for CREATE_SQ */
enum ena_admin_placement_policy_type {
	ENA_ADMIN_PLACEMENT_POLICY_HOST = 1,
	ENA_ADMIN_PLACEMENT_POLICY_DEV  = 3,
};

/* Completion policy for CREATE_SQ */
enum ena_admin_completion_policy_type {
	ENA_ADMIN_COMPLETION_POLICY_CQE = 0,
	ENA_ADMIN_COMPLETION_POLICY_CQE_ON_DEMAND = 1,
};

/* CREATE_CQ command and response descriptors */
struct ena_admin_aq_create_cq_cmd {
	struct ena_admin_aq_common_desc aq_common_descriptor;
	uint8_t cq_caps_1;       /* bit 5: interrupt_mode_enabled */
	uint8_t cq_caps_2;       /* bits 4:0: cq_entry_size_words */
	uint16_t cq_depth;       /* depth in entries, power of 2 */
	uint32_t msix_vector;
	struct ena_common_mem_addr cq_ba;
};

struct ena_admin_acq_create_cq_resp_desc {
	struct ena_admin_acq_common_desc acq_common_desc;
	uint16_t cq_idx;
	uint16_t cq_actual_depth;
	uint32_t numa_node_register_offset;
	uint32_t cq_head_db_register_offset;
	uint32_t cq_interrupt_unmask_register_offset;
};

/* DESTROY_CQ command and response descriptors */
struct ena_admin_aq_destroy_cq_cmd {
	struct ena_admin_aq_common_desc aq_common_descriptor;
	uint16_t cq_idx;
	uint16_t reserved1;
};

struct ena_admin_acq_destroy_cq_resp_desc {
	struct ena_admin_acq_common_desc acq_common_desc;
};

/* CREATE_SQ command and response descriptors */
struct ena_admin_aq_create_sq_cmd {
	struct ena_admin_aq_common_desc aq_common_descriptor;
	uint8_t sq_identity;     /* bits 7:5: direction (1=TX, 2=RX) */
	uint8_t reserved8_w1;
	uint8_t sq_caps_2;       /* bits 3:0: placement_policy, bits 6:4: completion_policy */
	uint8_t sq_caps_3;       /* bit 0: is_physically_contiguous */
	uint16_t cq_idx;
	uint16_t sq_depth;
	struct ena_common_mem_addr sq_ba;
	struct ena_common_mem_addr sq_head_writeback;
	uint32_t reserved0_w7;
	uint32_t reserved0_w8;
};

struct ena_admin_acq_create_sq_resp_desc {
	struct ena_admin_acq_common_desc acq_common_desc;
	uint16_t sq_idx;
	uint16_t reserved;
	uint32_t sq_doorbell_offset;
	uint32_t llq_descriptors_offset;
	uint32_t llq_headers_offset;
};

/* DESTROY_SQ command and response descriptors */
struct ena_admin_sq {
	uint16_t sq_idx;
	uint16_t reserved;
};

struct ena_admin_aq_destroy_sq_cmd {
	struct ena_admin_aq_common_desc aq_common_descriptor;
	struct ena_admin_sq sq;
};

struct ena_admin_acq_destroy_sq_resp_desc {
	struct ena_admin_acq_common_desc acq_common_desc;
};

/* Completion status values (0 = success). */
enum ena_admin_aq_completion_status {
	ENA_ADMIN_SUCCESS			= 0,
	ENA_ADMIN_RESOURCE_ALLOCATION_FAILURE = 1,
	ENA_ADMIN_BAD_OPCODE		= 2,
	ENA_ADMIN_UNSUPPORTED_OPCODE	= 3,
	ENA_ADMIN_MALFORMED_REQUEST	= 4,
	/* Additional status is provided in the ACQ extended_status field. */
	ENA_ADMIN_ILLEGAL_PARAMETER	= 5,
	ENA_ADMIN_UNKNOWN_ERROR		= 6,
	ENA_ADMIN_RESOURCE_BUSY		= 7,
};

/* AENQ event groups. */
enum ena_admin_aenq_group {
	ENA_ADMIN_LINK_CHANGE	= 0,
	ENA_ADMIN_FATAL_ERROR	= 1,
	ENA_ADMIN_WARNING		= 2,
	ENA_ADMIN_NOTIFICATION	= 3,
	ENA_ADMIN_KEEP_ALIVE	= 4,
};

/* AENQ event handler. Return 0 to accept the event. */
typedef int ena_aenq_handler(void *arg, uint16_t group, uint16_t syndrome,
			     const struct ena_admin_aenq_entry *entry);

#ifndef __Unikraft__
/* Host test hook: the mock registers a callback to emulate the device
 * reaction to an admin queue doorbell write. */
typedef void ena_admin_db_hook(void *cookie, uint16_t tail);
void ena_admin_set_db_hook(ena_admin_db_hook *hook, void *cookie);
#endif

#endif /* LIBENA_ENA_ADMIN_H */
