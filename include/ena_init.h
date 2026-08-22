/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_INIT_H
#define LIBENA_ENA_INIT_H

#include <stdint.h>
#include <stddef.h>

#include "ena_admin.h"

/* Forward declaration */
struct ena_adapter;

/* Feature IDs for the get/set feature admin commands
 * (reference/ena_admin_defs.h, enum ena_admin_aq_feature_id). */
enum ena_admin_aq_feature_id {
	ENA_ADMIN_DEVICE_ATTRIBUTES	= 1,
	ENA_ADMIN_MAX_QUEUES_NUM	= 2,
	ENA_ADMIN_LLQ			= 4,
	ENA_ADMIN_MTU			= 14,
	ENA_ADMIN_HOST_ATTR_CONFIG	= 28,
};

/* OS types (reference/ena_admin_defs.h, enum ena_admin_os_type). */
enum ena_admin_os_type {
	ENA_ADMIN_OS_LINUX	= 1,
	ENA_ADMIN_OS_WIN	= 2,
	ENA_ADMIN_OS_DPDK	= 3,
	ENA_ADMIN_OS_FREEBSD	= 4,
	ENA_ADMIN_OS_IPXE	= 5,
	ENA_ADMIN_OS_ESXI	= 6,
};

/* Driver identity reported to the device. */
#define ENA_INIT_OS_NAME		"Unikraft"
#define ENA_INIT_DRIVER_NAME		"ena-unikraft"
#define ENA_INIT_DRIVER_VERSION_MAJOR	1
#define ENA_INIT_DRIVER_VERSION_MINOR	0
#define ENA_INIT_DRIVER_VERSION_SUBMINOR 0
#define ENA_INIT_DRIVER_MODULE_TYPE	0
#define ENA_INIT_ENA_SPEC_VERSION	2

/* Pack the driver version into the u32 field (reference bit layout). */
#define ENA_INIT_PACK_DRIVER_VERSION(major, minor, sub, module) \
	(((uint32_t)(major) & 0xFFu) | \
	 (((uint32_t)(minor) & 0xFFu) << 8) | \
	 (((uint32_t)(sub) & 0xFFu) << 16) | \
	 (((uint32_t)(module) & 0xFFu) << 24))

/* Host info buffer size: 4KB of physically contiguous memory. */
#define ENA_INIT_HOST_INFO_SIZE	4096

/* Minimum legal Ethernet MTU (L3, excluding L2). */
#define ENA_INIT_MTU_MIN	46

/* Maximum MTU this driver negotiates. */
#define ENA_INIT_MTU_MAX	0xFFFF

/* Select field (bits 1:0) of the common descriptor.
 * 0x1: select the current value. 0x3: select the default value. */
#define ENA_ADMIN_FEAT_SELECT_CURRENT 0x1u
#define ENA_ADMIN_FEAT_SELECT_DEFAULT 0x3u

/* Common descriptor of the get/set feature admin commands. */
struct ena_admin_get_set_feature_common_desc {
	uint8_t flags;           /* bits 1:0: select */
	uint8_t feature_id;      /* enum ena_admin_aq_feature_id */
	uint8_t feature_version;
	uint8_t reserved8;
};

/* GET_FEATURE response for ENA_ADMIN_DEVICE_ATTRIBUTES. */
struct ena_admin_device_attr_feature_desc {
	uint32_t impl_id;
	uint32_t device_version;
	uint32_t supported_features;
	uint32_t capabilities;
	uint32_t phys_addr_width;
	uint32_t virt_addr_width;
	uint8_t mac_addr[6];
	uint8_t reserved7[2];
	uint32_t max_mtu;
};

/* GET_FEATURE response for ENA_ADMIN_MAX_QUEUES_NUM. */
struct ena_admin_queue_feature_desc {
	uint32_t max_sq_num;
	uint32_t max_sq_depth;
	uint32_t max_cq_num;
	uint32_t max_cq_depth;
	uint32_t max_legacy_llq_num;
	uint32_t max_legacy_llq_depth;
	uint32_t max_header_size;
	uint16_t max_packet_tx_descs;
	uint16_t max_packet_rx_descs;
};

/* SET_FEATURE payload for ENA_ADMIN_MTU. */
struct ena_admin_set_feature_mtu_desc {
	uint32_t mtu;
};

/* SET_FEATURE payload for ENA_ADMIN_HOST_ATTR_CONFIG. */
struct ena_admin_set_feature_host_attr_desc {
	struct ena_common_mem_addr os_info_ba;
	struct ena_common_mem_addr debug_ba;
	uint32_t debug_area_size;
};

/* 4KB host info buffer (reference/ena_admin_defs.h, ena_admin_host_info). */
struct ena_admin_host_info {
	uint32_t os_type;
	uint8_t os_dist_str[128];
	uint32_t os_dist;
	uint8_t kernel_ver_str[32];
	uint32_t kernel_ver;
	uint32_t driver_version;
	uint32_t supported_network_features[2];
	uint16_t ena_spec_version;
	uint16_t bdf;
	uint16_t num_cpus;
	uint16_t reserved;
	uint32_t driver_supported_features;
};

/* Inline payloads of the get/set feature commands.
 * They fill the 60-byte inline data region of an AQ entry. */

struct ena_admin_get_feat_inline {
	struct ena_admin_ctrl_buff_info control_buffer;
	struct ena_admin_get_set_feature_common_desc feat_common;
	uint32_t raw[11];
};

struct ena_admin_set_feat_mtu_inline {
	struct ena_admin_ctrl_buff_info control_buffer;
	struct ena_admin_get_set_feature_common_desc feat_common;
	struct ena_admin_set_feature_mtu_desc mtu;
};

struct ena_admin_set_feat_host_inline {
	struct ena_admin_ctrl_buff_info control_buffer;
	struct ena_admin_get_set_feature_common_desc feat_common;
	struct ena_admin_set_feature_host_attr_desc host_attr;
};

/* Phase 3: device initialization and capability negotiation. */
int ena_init_get_device_attributes(struct ena_adapter *adapter);
int ena_init_get_queue_limits(struct ena_adapter *adapter);
int ena_init_set_host_info(struct ena_adapter *adapter);
int ena_init_set_mtu(struct ena_adapter *adapter, uint32_t mtu);
int ena_init_get_mac_addr(struct ena_adapter *adapter, uint8_t mac[6]);
int ena_init_run(struct ena_adapter *adapter, uint32_t mtu);

#endif /* LIBENA_ENA_INIT_H */
