/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_RSS_H
#define LIBENA_ENA_RSS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ena_admin.h"
#include "ena_init.h"

struct ena_adapter;

/* Number of 32-bit words in the Toeplitz hash key (40 bytes total). */
#define ENA_ADMIN_RSS_KEY_PARTS 10

/* Default (fallback) indirection table size (power of 2). Used when the
 * device does not report a supported size range via GET_FEATURE. */
#define ENA_ADMIN_RSS_IND_TABLE_NUM_ENTRIES 128

/* RSS hash functions (reference/ena_admin_defs.h). */
enum ena_admin_hash_functions {
	ENA_ADMIN_TOEPLITZ = 1,
	ENA_ADMIN_CRC32 = 2,
};

/* RSS flow hash protocols (reference/ena_admin_defs.h). */
enum ena_admin_flow_hash_proto {
	ENA_ADMIN_RSS_TCP4 = 0,
	ENA_ADMIN_RSS_UDP4 = 1,
	ENA_ADMIN_RSS_TCP6 = 2,
	ENA_ADMIN_RSS_UDP6 = 3,
	ENA_ADMIN_RSS_IP4 = 4,
	ENA_ADMIN_RSS_IP6 = 5,
	ENA_ADMIN_RSS_IP4_FRAG = 6,
	ENA_ADMIN_RSS_NOT_IP = 7,
	ENA_ADMIN_RSS_TCP6_EX = 8,
	ENA_ADMIN_RSS_IP6_EX = 9,
	ENA_ADMIN_RSS_PROTO_NUM = 16,
};

/* RSS flow hash fields bitmask (reference/ena_admin_defs.h). */
enum ena_admin_flow_hash_fields {
	ENA_ADMIN_RSS_L2_DA = (1u << 0),
	ENA_ADMIN_RSS_L2_SA = (1u << 1),
	ENA_ADMIN_RSS_L3_DA = (1u << 2),
	ENA_ADMIN_RSS_L3_SA = (1u << 3),
	ENA_ADMIN_RSS_L4_DP = (1u << 4),
	ENA_ADMIN_RSS_L4_SP = (1u << 5),
};

/* Wire structures for RSS feature commands. */

/* Hash key buffer for ENA_ADMIN_RSS_HASH_FUNCTION. */
struct ena_admin_feature_rss_flow_hash_control {
	uint32_t key_parts;
	uint32_t reserved;
	uint32_t key[ENA_ADMIN_RSS_KEY_PARTS];
};

/* Hash function descriptor for ENA_ADMIN_RSS_HASH_FUNCTION. */
struct ena_admin_feature_rss_flow_hash_function {
	uint32_t supported_func;
	uint32_t selected_func;
	uint32_t init_val;
};

/* Hash input descriptor for ENA_ADMIN_RSS_HASH_INPUT. */
struct ena_admin_feature_rss_flow_hash_input {
	uint16_t supported_input_sort;
	uint16_t enabled_input_sort;
};

#define ENA_ADMIN_FEATURE_RSS_FLOW_HASH_INPUT_L3_SORT_MASK 0x02
#define ENA_ADMIN_FEATURE_RSS_FLOW_HASH_INPUT_L4_SORT_MASK 0x04

/* Flow hash field entry per protocol. */
struct ena_admin_proto_input {
	uint16_t fields;
	uint16_t reserved2;
};

/* Flow hash control buffer. */
struct ena_admin_feature_rss_hash_control {
	struct ena_admin_proto_input supported_fields[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input selected_fields[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input reserved2[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input reserved3[ENA_ADMIN_RSS_PROTO_NUM];
};

/* Single entry in the hardware indirection table. Per the firmware
 * spec, sq_idx holds the 0-based RX SQ index returned by CREATE_SQ. */
struct ena_admin_rss_ind_table_entry {
	uint16_t sq_idx;
	uint16_t reserved;
};

/* Indirection table descriptor for ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG. */
struct ena_admin_feature_rss_ind_table {
	uint16_t min_size;
	uint16_t max_size;
	uint16_t size;
	uint16_t reserved;
	uint32_t inline_index;
	struct ena_admin_rss_ind_table_entry inline_entry;
};

/* Software state for RSS on an adapter. */
struct ena_rss_info {
	bool supported;
	bool enabled;
	uint16_t ind_table_size;
	uint16_t *host_ind_table;
	struct ena_admin_rss_ind_table_entry *ind_table;
	uint64_t ind_table_phys;
	struct ena_admin_feature_rss_flow_hash_control *hash_key;
	uint64_t hash_key_phys;
	struct ena_admin_feature_rss_hash_control *hash_ctrl;
	uint64_t hash_ctrl_phys;
};

#if !defined(__Unikraft__) || defined(CONFIG_LIBENA_RSS)

/**
 * Initialize RSS software structures and allocate DMA control buffers.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rss_init(struct ena_adapter *adapter);

/**
 * Free RSS DMA buffers and software state.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 */
void ena_rss_fini(struct ena_adapter *adapter);

/**
 * Program the Toeplitz hash key to the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param key Pointer to 40-byte key buffer (or NULL for standard default).
 * @param key_len Length of key in bytes (must be 40 bytes if key != NULL).
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rss_set_hash_key(struct ena_adapter *adapter, const uint8_t *key,
			 size_t key_len);

/**
 * Program the flow hash fields (4-tuple for TCP/IPv4) to the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rss_set_hash_ctrl(struct ena_adapter *adapter);

/**
 * Populate and program the indirection table distributing over num_queues.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param num_queues Number of active RX queues to distribute over.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rss_set_ind_table(struct ena_adapter *adapter, uint16_t num_queues);

/**
 * Run full RSS configuration sequence (hash key, hash fields, indirection
 * table).
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param num_queues Number of active RX queues to distribute over.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rss_configure(struct ena_adapter *adapter, uint16_t num_queues);

#else /* defined(__Unikraft__) && !defined(CONFIG_LIBENA_RSS) */

static inline int ena_rss_init(struct ena_adapter *adapter)
{
	(void)adapter;
	return 0;
}

static inline void ena_rss_fini(struct ena_adapter *adapter)
{
	(void)adapter;
}

static inline int ena_rss_set_hash_key(struct ena_adapter *adapter,
				       const uint8_t *key, size_t key_len)
{
	(void)adapter;
	(void)key;
	(void)key_len;
	return 0;
}

static inline int ena_rss_set_hash_ctrl(struct ena_adapter *adapter)
{
	(void)adapter;
	return 0;
}

static inline int ena_rss_set_ind_table(struct ena_adapter *adapter,
					uint16_t num_queues)
{
	(void)adapter;
	(void)num_queues;
	return 0;
}

static inline int ena_rss_configure(struct ena_adapter *adapter,
				    uint16_t num_queues)
{
	(void)adapter;
	(void)num_queues;
	return 0;
}

#endif /* !CONFIG_LIBENA_RSS */

#endif /* LIBENA_ENA_RSS_H */
