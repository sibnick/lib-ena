/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_rss.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* Bounded poll budget for RSS admin commands (500ms at 100us per poll). */
#define ENA_RSS_MAX_POLLS 5000

/* Standard Microsoft Toeplitz 40-byte RSS hash key */
static const uint8_t default_toeplitz_key[ENA_ADMIN_RSS_KEY_PARTS * sizeof(uint32_t)] = {
	0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
	0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
	0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
	0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
	0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa
};

static int ena_rss_exec(struct ena_adapter *adapter, uint8_t opcode,
			const void *req, size_t req_len, void *resp,
			size_t resp_cap)
{
	return ena_admin_exec_cmd(adapter, opcode, req, req_len, resp, resp_cap,
				  NULL, ENA_RSS_MAX_POLLS);
}

int ena_rss_init(struct ena_adapter *adapter)
{
	struct ena_rss_info *rss;
	size_t ind_tbl_size;
	size_t key_size;
	size_t ctrl_size;

	if (!adapter)
		return -EINVAL;

	if (!(adapter->supported_features & (1u << ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG))) {
		ena_warn("rss: device does not support indirection table (features=0x%x)",
			 adapter->supported_features);
		return -EOPNOTSUPP;
	}

	rss = &adapter->rss_info;
	if (rss->host_ind_table && rss->ind_table)
		return 0;

	rss->ind_table_size = ENA_ADMIN_RSS_IND_TABLE_NUM_ENTRIES;

	rss->host_ind_table = calloc(rss->ind_table_size, sizeof(uint16_t));
	if (!rss->host_ind_table)
		return -ENOMEM;

	ind_tbl_size = (size_t)rss->ind_table_size * sizeof(struct ena_admin_rss_ind_table_entry);
	rss->ind_table = (struct ena_admin_rss_ind_table_entry *)
		ena_dma_alloc(ind_tbl_size, &rss->ind_table_phys);
	if (!rss->ind_table) {
		free(rss->host_ind_table);
		rss->host_ind_table = NULL;
		return -ENOMEM;
	}
	memset(rss->ind_table, 0, ind_tbl_size);

	if (adapter->supported_features & (1u << ENA_ADMIN_RSS_HASH_FUNCTION)) {
		key_size = sizeof(struct ena_admin_feature_rss_flow_hash_control);
		rss->hash_key = (struct ena_admin_feature_rss_flow_hash_control *)
			ena_dma_alloc(key_size, &rss->hash_key_phys);
		if (!rss->hash_key) {
			ena_rss_fini(adapter);
			return -ENOMEM;
		}
		memset(rss->hash_key, 0, key_size);
	}

	if (adapter->supported_features & (1u << ENA_ADMIN_RSS_HASH_INPUT)) {
		ctrl_size = sizeof(struct ena_admin_feature_rss_hash_control);
		rss->hash_ctrl = (struct ena_admin_feature_rss_hash_control *)
			ena_dma_alloc(ctrl_size, &rss->hash_ctrl_phys);
		if (!rss->hash_ctrl) {
			ena_rss_fini(adapter);
			return -ENOMEM;
		}
		memset(rss->hash_ctrl, 0, ctrl_size);
	}

	rss->supported = true;
	return 0;
}

void ena_rss_fini(struct ena_adapter *adapter)
{
	struct ena_rss_info *rss;

	if (!adapter)
		return;

	rss = &adapter->rss_info;

	if (rss->host_ind_table) {
		free(rss->host_ind_table);
		rss->host_ind_table = NULL;
	}

	if (rss->ind_table) {
		ena_dma_free(rss->ind_table, rss->ind_table_phys);
		rss->ind_table = NULL;
		rss->ind_table_phys = 0;
	}

	if (rss->hash_key) {
		ena_dma_free(rss->hash_key, rss->hash_key_phys);
		rss->hash_key = NULL;
		rss->hash_key_phys = 0;
	}

	if (rss->hash_ctrl) {
		ena_dma_free(rss->hash_ctrl, rss->hash_ctrl_phys);
		rss->hash_ctrl = NULL;
		rss->hash_ctrl_phys = 0;
	}

	rss->supported = false;
	rss->enabled = false;
}

int ena_rss_set_hash_key(struct ena_adapter *adapter, const uint8_t *key, size_t key_len)
{
	struct ena_rss_info *rss;
	struct {
		struct ena_admin_ctrl_buff_info control_buffer;
		struct ena_admin_get_set_feature_common_desc feat_common;
		struct ena_admin_feature_rss_flow_hash_function flow_hash_func;
	} req;
	const uint8_t *src_key;
	int ret;

	if (!adapter)
		return -EINVAL;

	rss = &adapter->rss_info;
	if (!rss->hash_key)
		return -EINVAL;

	if (key && key_len != sizeof(default_toeplitz_key))
		return -EINVAL;

	src_key = key ? key : default_toeplitz_key;

	rss->hash_key->key_parts = ENA_ADMIN_RSS_KEY_PARTS;
	rss->hash_key->reserved = 0;
	memcpy(rss->hash_key->key, src_key, sizeof(default_toeplitz_key));

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_RSS_HASH_FUNCTION;
	req.flow_hash_func.selected_func = (1u << ENA_ADMIN_TOEPLITZ);
	req.flow_hash_func.init_val = 0;

	req.control_buffer.length = sizeof(struct ena_admin_feature_rss_flow_hash_control);
	req.control_buffer.address.mem_addr_low = (uint32_t)(rss->hash_key_phys & 0xFFFFFFFFu);
	req.control_buffer.address.mem_addr_high = (uint16_t)((rss->hash_key_phys >> 32) & 0xFFFFu);

	ret = ena_rss_exec(adapter, ENA_ADMIN_SET_FEATURE, &req, sizeof(req), NULL, 0);
	if (ret) {
		ena_warn("rss: set hash key failed (%d)", ret);
		return ret;
	}

	ena_info("rss: configured 40-byte Toeplitz hash key");
	return 0;
}

int ena_rss_set_hash_ctrl(struct ena_adapter *adapter)
{
	struct ena_rss_info *rss;
	struct {
		struct ena_admin_ctrl_buff_info control_buffer;
		struct ena_admin_get_set_feature_common_desc feat_common;
		struct ena_admin_feature_rss_flow_hash_input flow_hash_input;
	} req;
	uint16_t tcp_udp_fields;
	uint16_t ip_fields;
	int ret;

	if (!adapter)
		return -EINVAL;

	rss = &adapter->rss_info;
	if (!rss->hash_ctrl)
		return -EINVAL;

	memset(rss->hash_ctrl, 0, sizeof(*rss->hash_ctrl));

	/* IPv4 TCP and UDP: 4-tuple (Src IP, Dst IP, Src Port, Dst Port) */
	tcp_udp_fields = (uint16_t)(ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L3_DA |
				    ENA_ADMIN_RSS_L4_SP | ENA_ADMIN_RSS_L4_DP);
	rss->hash_ctrl->selected_fields[ENA_ADMIN_RSS_TCP4].fields = tcp_udp_fields;
	rss->hash_ctrl->selected_fields[ENA_ADMIN_RSS_UDP4].fields = tcp_udp_fields;

	/* IPv4 general: 2-tuple (Src IP, Dst IP) */
	ip_fields = (uint16_t)(ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L3_DA);
	rss->hash_ctrl->selected_fields[ENA_ADMIN_RSS_IP4].fields = ip_fields;
	rss->hash_ctrl->selected_fields[ENA_ADMIN_RSS_IP4_FRAG].fields = ip_fields;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_RSS_HASH_INPUT;
	req.flow_hash_input.enabled_input_sort =
		ENA_ADMIN_FEATURE_RSS_FLOW_HASH_INPUT_L3_SORT_MASK |
		ENA_ADMIN_FEATURE_RSS_FLOW_HASH_INPUT_L4_SORT_MASK;

	req.control_buffer.length = sizeof(struct ena_admin_feature_rss_hash_control);
	req.control_buffer.address.mem_addr_low = (uint32_t)(rss->hash_ctrl_phys & 0xFFFFFFFFu);
	req.control_buffer.address.mem_addr_high = (uint16_t)((rss->hash_ctrl_phys >> 32) & 0xFFFFu);

	ret = ena_rss_exec(adapter, ENA_ADMIN_SET_FEATURE, &req, sizeof(req), NULL, 0);
	if (ret) {
		ena_warn("rss: set hash control failed (%d)", ret);
		return ret;
	}

	ena_info("rss: configured TCP4/UDP4 4-tuple flow hashing");
	return 0;
}

int ena_rss_set_ind_table(struct ena_adapter *adapter, uint16_t num_queues)
{
	struct ena_rss_info *rss;
	struct {
		struct ena_admin_ctrl_buff_info control_buffer;
		struct ena_admin_get_set_feature_common_desc feat_common;
		struct ena_admin_feature_rss_ind_table ind_table;
	} req;
	uint16_t i;
	int ret;

	if (!adapter || num_queues == 0)
		return -EINVAL;

	rss = &adapter->rss_info;
	if (!rss->ind_table || !rss->host_ind_table)
		return -EINVAL;

	/* Populate indirection table entries round-robin across active queues */
	for (i = 0; i < rss->ind_table_size; i++) {
		uint16_t target_q = (uint16_t)(i % num_queues);
		rss->host_ind_table[i] = target_q;
		rss->ind_table[i].cq_idx = target_q;
		rss->ind_table[i].reserved = 0;
	}

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG;
	req.ind_table.size = (uint16_t)__builtin_ctz(rss->ind_table_size);
	req.ind_table.inline_index = 0xFFFFFFFFu; /* Set entire table via control buffer */

	req.control_buffer.length = (uint32_t)(rss->ind_table_size *
					      sizeof(struct ena_admin_rss_ind_table_entry));
	req.control_buffer.address.mem_addr_low = (uint32_t)(rss->ind_table_phys & 0xFFFFFFFFu);
	req.control_buffer.address.mem_addr_high = (uint16_t)((rss->ind_table_phys >> 32) & 0xFFFFu);

	ret = ena_rss_exec(adapter, ENA_ADMIN_SET_FEATURE, &req, sizeof(req), NULL, 0);
	if (ret) {
		ena_warn("rss: set indirection table failed (%d)", ret);
		return ret;
	}

	ena_info("rss: indirection table (%u entries) configured across %u queues",
		 rss->ind_table_size, num_queues);
	return 0;
}

int ena_rss_configure(struct ena_adapter *adapter, uint16_t num_queues)
{
	int ret;

	if (!adapter || num_queues == 0)
		return -EINVAL;

	ret = ena_rss_init(adapter);
	if (ret)
		return ret;

	if (adapter->supported_features & (1u << ENA_ADMIN_RSS_HASH_FUNCTION)) {
		ret = ena_rss_set_hash_key(adapter, NULL, 0);
		if (ret)
			return ret;
	}

	if (adapter->supported_features & (1u << ENA_ADMIN_RSS_HASH_INPUT)) {
		ret = ena_rss_set_hash_ctrl(adapter);
		if (ret)
			return ret;
	}

	ret = ena_rss_set_ind_table(adapter, num_queues);
	if (ret)
		return ret;

	adapter->rss_info.enabled = true;
	return 0;
}
