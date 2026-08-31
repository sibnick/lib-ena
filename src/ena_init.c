/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"

#include <errno.h>
#include <string.h>

/* Bounded poll budget for init commands (500ms at 100us per poll). */
#define ENA_INIT_MAX_POLLS 5000

static size_t ena_copy_str(char *dst, size_t cap, const char *src)
{
	size_t len = strlen(src);

	if (len >= cap)
		len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return len;
}

static int ena_init_exec(struct ena_adapter *adapter, uint8_t opcode,
			 const void *req, size_t req_len, void *resp,
			 size_t resp_cap)
{
	return ena_admin_exec_cmd(adapter, opcode, req, req_len, resp, resp_cap,
				  NULL, ENA_INIT_MAX_POLLS);
}

int ena_init_get_device_attributes(struct ena_adapter *adapter)
{
	struct ena_admin_get_feat_inline req;
	const struct ena_admin_device_attr_feature_desc *attr;
	uint32_t resp[14];
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_DEVICE_ATTRIBUTES;

	ret = ena_init_exec(adapter, ENA_ADMIN_GET_FEATURE, &req, sizeof(req),
			    resp, sizeof(resp));
	if (ret)
		return ret;

	attr = (const struct ena_admin_device_attr_feature_desc *)resp;

	adapter->impl_id = attr->impl_id;
	adapter->device_version = attr->device_version;
	adapter->supported_features = attr->supported_features;
	adapter->attr_caps = attr->capabilities;
	adapter->phys_addr_width = attr->phys_addr_width;
	adapter->virt_addr_width = attr->virt_addr_width;
	memcpy(adapter->mac_addr, attr->mac_addr, 6);
	adapter->max_mtu = (uint16_t)attr->max_mtu;

	return 0;
}

#define ENA_SPEC_MAX_QUEUES 256
#define ENA_SPEC_MAX_DEPTH  4096
#define ENA_SPEC_MIN_DEPTH  4

static uint16_t clamp_queue_depth(uint32_t raw_depth)
{
	uint32_t d = raw_depth;
	if (d > ENA_SPEC_MAX_DEPTH)
		d = ENA_SPEC_MAX_DEPTH;
	if (d < ENA_SPEC_MIN_DEPTH)
		d = ENA_SPEC_MIN_DEPTH;
	/* Round down to power of two */
	while ((d & (d - 1)) != 0)
		d = d & (d - 1);
	return (uint16_t)d;
}

static uint16_t clamp_queue_num(uint32_t raw_num)
{
	if (raw_num > ENA_SPEC_MAX_QUEUES)
		return ENA_SPEC_MAX_QUEUES;
	if (raw_num == 0)
		return 1;
	return (uint16_t)raw_num;
}

int ena_init_get_queue_limits(struct ena_adapter *adapter)
{
	struct ena_admin_get_feat_inline req;
	const struct ena_admin_queue_feature_desc *q;
	uint32_t resp[14];
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_MAX_QUEUES_NUM;

	ret = ena_init_exec(adapter, ENA_ADMIN_GET_FEATURE, &req, sizeof(req),
			    resp, sizeof(resp));
	if (ret)
		return ret;

	uint16_t max_tx_q;
	uint16_t max_rx_q;
	uint16_t max_tx_depth;
	uint16_t max_rx_depth;
	uint32_t raw_sq_num;
	uint32_t raw_cq_num;
	uint32_t raw_sq_depth;
	uint32_t raw_cq_depth;

	q = (const struct ena_admin_queue_feature_desc *)resp;

	raw_sq_num = ena_le32_to_cpu(q->max_sq_num);
	raw_cq_num = ena_le32_to_cpu(q->max_cq_num);
	raw_sq_depth = ena_le32_to_cpu(q->max_sq_depth);
	raw_cq_depth = ena_le32_to_cpu(q->max_cq_depth);

	max_tx_q = clamp_queue_num(raw_sq_num);
	max_rx_q = clamp_queue_num(raw_sq_num);
	if (clamp_queue_num(raw_cq_num) < max_tx_q)
		max_tx_q = clamp_queue_num(raw_cq_num);
	if (clamp_queue_num(raw_cq_num) < max_rx_q)
		max_rx_q = clamp_queue_num(raw_cq_num);

	max_tx_depth = clamp_queue_depth(raw_sq_depth);
	max_rx_depth = clamp_queue_depth(raw_sq_depth);
	if (clamp_queue_depth(raw_cq_depth) < max_tx_depth)
		max_tx_depth = clamp_queue_depth(raw_cq_depth);
	if (clamp_queue_depth(raw_cq_depth) < max_rx_depth)
		max_rx_depth = clamp_queue_depth(raw_cq_depth);

	if (raw_sq_num > ENA_SPEC_MAX_QUEUES || raw_cq_num > ENA_SPEC_MAX_QUEUES ||
	    raw_sq_depth > ENA_SPEC_MAX_DEPTH || raw_cq_depth > ENA_SPEC_MAX_DEPTH) {
		ena_info("queue limits clamped from raw sq_num=%u cq_num=%u sq_depth=%u cq_depth=%u",
			 raw_sq_num, raw_cq_num, raw_sq_depth, raw_cq_depth);
	}

	adapter->max_tx_queues = max_tx_q;
	adapter->max_rx_queues = max_rx_q;
	adapter->max_tx_ring_size = max_tx_depth;
	adapter->max_rx_ring_size = max_rx_depth;
	adapter->max_header_size = ena_le32_to_cpu(q->max_header_size);
	adapter->max_packet_tx_descs = q->max_packet_tx_descs;
	adapter->max_packet_rx_descs = q->max_packet_rx_descs;

	ena_info("queue feature: max_tx_q=%u max_rx_q=%u max_tx_depth=%u max_rx_depth=%u",
		 max_tx_q, max_rx_q, max_tx_depth, max_rx_depth);

	return 0;
}

int ena_init_set_host_info(struct ena_adapter *adapter)
{
	struct ena_admin_set_feat_host_inline req;
	struct ena_admin_host_info *info;
	uint64_t phys = 0;
	uint8_t *base;
	int ret;

	if (!adapter)
		return -EINVAL;

	base = ena_dma_alloc(ENA_INIT_HOST_INFO_SIZE, &phys);
	if (!base)
		return -ENOMEM;

	memset(base, 0, ENA_INIT_HOST_INFO_SIZE);
	info = (struct ena_admin_host_info *)base;
	info->os_type = ENA_ADMIN_OS_DPDK;
	ena_copy_str((char *)info->os_dist_str, sizeof(info->os_dist_str),
		     ENA_INIT_OS_NAME);
	ena_copy_str((char *)info->kernel_ver_str, sizeof(info->kernel_ver_str),
		     ENA_INIT_DRIVER_NAME);
	info->driver_version = ENA_INIT_PACK_DRIVER_VERSION(
		ENA_INIT_DRIVER_VERSION_MAJOR, ENA_INIT_DRIVER_VERSION_MINOR,
		ENA_INIT_DRIVER_VERSION_SUBMINOR, ENA_INIT_DRIVER_MODULE_TYPE);
	info->ena_spec_version = ENA_INIT_ENA_SPEC_VERSION;
	info->num_cpus = 1;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_HOST_ATTR_CONFIG;
	req.host_attr.os_info_ba.mem_addr_low =
		(uint32_t)(phys & 0xFFFFFFFFu);
	req.host_attr.os_info_ba.mem_addr_high =
		(uint16_t)((phys >> 32) & 0xFFFFu);

	ret = ena_init_exec(adapter, ENA_ADMIN_SET_FEATURE, &req, sizeof(req),
			    NULL, 0);
	if (ret) {
		ena_dma_free(base, phys);
		return ret;
	}

	adapter->host_info_base = base;
	adapter->host_info_phys = phys;

	return 0;
}

int ena_init_set_mtu(struct ena_adapter *adapter, uint32_t mtu)
{
	struct ena_admin_set_feat_mtu_inline req;
	int ret;

	if (!adapter)
		return -EINVAL;

	/* The device enforces its own maximum; this is the legal range only. */
	if (mtu < ENA_INIT_MTU_MIN || mtu > ENA_INIT_MTU_MAX)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	req.feat_common.feature_id = ENA_ADMIN_MTU;
	req.mtu.mtu = mtu;

	ret = ena_init_exec(adapter, ENA_ADMIN_SET_FEATURE, &req, sizeof(req),
			    NULL, 0);
	if (ret)
		return ret;

	adapter->mtu = mtu;

	return 0;
}

int ena_init_get_mac_addr(struct ena_adapter *adapter, uint8_t mac[6])
{
	int ret;

	if (!adapter || !mac)
		return -EINVAL;

	/* The MAC address is a field of the device attributes response. */
	ret = ena_init_get_device_attributes(adapter);
	if (ret)
		return ret;

	if (mac != adapter->mac_addr)
		memcpy(mac, adapter->mac_addr, 6);

	return 0;
}

int ena_init_config_llq(struct ena_adapter *adapter)
{
	struct ena_admin_get_feat_inline get_req;
	struct ena_admin_feature_llq_desc llq;
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&get_req, 0, sizeof(get_req));
	get_req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	get_req.feat_common.feature_id = ENA_ADMIN_LLQ;

	ret = ena_init_exec(adapter, ENA_ADMIN_GET_FEATURE, &get_req, sizeof(get_req),
			    &llq, sizeof(llq));
	if (ret) {
		ena_info("LLQ: not supported by device (%d)", ret);
		return 0;
	}

	ena_info("LLQ: max_llq_num=%u max_llq_depth=%u header_loc=0x%x entry_size=0x%x",
		 llq.max_llq_num, llq.max_llq_depth,
		 llq.header_location_ctrl_supported, llq.entry_size_ctrl_supported);

	if (llq.max_llq_num == 0)
		return 0;

	struct {
		struct ena_admin_ctrl_buff_info control_buffer;
		struct ena_admin_get_set_feature_common_desc feat_common;
		struct ena_admin_feature_llq_desc llq;
	} set_req;

	memset(&set_req, 0, sizeof(set_req));
	set_req.feat_common.flags = ENA_ADMIN_FEAT_SELECT_CURRENT;
	set_req.feat_common.feature_id = ENA_ADMIN_LLQ;
	set_req.llq = llq;

	if (llq.header_location_ctrl_supported & 1)
		set_req.llq.header_location_ctrl_enabled = 1;
	if (llq.entry_size_ctrl_supported & 1)
		set_req.llq.entry_size_ctrl_enabled = 1;
	if (llq.desc_num_before_header_supported & 1)
		set_req.llq.desc_num_before_header_enabled = 1;
	if (llq.descriptors_stride_ctrl_supported & 1)
		set_req.llq.descriptors_stride_ctrl_enabled = 1;

	ret = ena_init_exec(adapter, ENA_ADMIN_SET_FEATURE, &set_req, sizeof(set_req),
			    NULL, 0);
	if (ret) {
		ena_warn("LLQ: set feature failed (%d)", ret);
		return 0;
	}

	adapter->llq_info.supported = true;
	adapter->llq_info.enabled = true;
	adapter->llq_info.max_llq_num = llq.max_llq_num;
	adapter->llq_info.max_llq_depth = llq.max_llq_depth;
	adapter->llq_info.entry_size = 128;
	adapter->llq_info.header_len = 96;

	ena_info("LLQ: enabled (entry_size=128)");
	return 0;
}

int ena_init_run(struct ena_adapter *adapter, uint32_t mtu)
{
	int ret;

	if (!adapter)
		return -EINVAL;

	ret = ena_init_get_device_attributes(adapter);
	if (ret)
		return ret;

	ret = ena_init_get_queue_limits(adapter);
	if (ret)
		return ret;

	ret = ena_init_set_host_info(adapter);
	if (ret)
		return ret;

	ret = ena_init_config_llq(adapter);
	if (ret)
		return ret;

	ret = ena_init_set_mtu(adapter, mtu);
	if (ret)
		return ret;

	ret = ena_init_get_mac_addr(adapter, adapter->mac_addr);
	if (ret)
		return ret;

	adapter->state = ENA_STATE_CONFIGURED;
	ena_info("init: mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x", adapter->mtu,
		 adapter->mac_addr[0], adapter->mac_addr[1],
		 adapter->mac_addr[2], adapter->mac_addr[3],
		 adapter->mac_addr[4], adapter->mac_addr[5]);
	return 0;
}
