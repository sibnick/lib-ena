/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int setup_adapter(struct mock_ena_hw *hw, struct ena_adapter *adapter)
{
	mock_ena_hw_init(hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, hw);

	int ret = ena_device_init_scaffold(adapter, hw->bar0, sizeof(hw->bar0));
	if (ret)
		return ret;

	return ena_admin_init(adapter, 8, 8, 8);
}

static void test_init_wire_layouts(void)
{
	printf("[TEST] Running test_init_wire_layouts...\n");

	/* The wire structs must match the reference layouts. */
	assert(sizeof(struct ena_admin_get_set_feature_common_desc) == 4);
	assert(offsetof(struct ena_admin_get_set_feature_common_desc,
			feature_id) == 1);

	assert(sizeof(struct ena_admin_device_attr_feature_desc) == 36);
	assert(offsetof(struct ena_admin_device_attr_feature_desc, mac_addr) == 24);
	assert(offsetof(struct ena_admin_device_attr_feature_desc, max_mtu) == 32);

	assert(sizeof(struct ena_admin_queue_feature_desc) == 32);
	assert(offsetof(struct ena_admin_queue_feature_desc,
			max_packet_tx_descs) == 28);

	assert(sizeof(struct ena_admin_set_feature_mtu_desc) == 4);
	assert(sizeof(struct ena_admin_set_feature_host_attr_desc) == 20);
	assert(sizeof(struct ena_admin_host_info) == 196);

	/* Inline payloads fit the 60-byte AQ inline data region. */
	assert(sizeof(struct ena_admin_get_feat_inline) == 60);
	assert(sizeof(struct ena_admin_set_feat_mtu_inline) == 20);
	assert(sizeof(struct ena_admin_set_feat_host_inline) == 36);
	printf("[PASS] test_init_wire_layouts passed\n");
}

static void test_init_device_attributes(void)
{
	printf("[TEST] Running test_init_device_attributes...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);

	assert(ena_init_get_device_attributes(&adapter) == 0);
	assert(hw.attrs_read == 1);

	assert(adapter.impl_id == 0x1D0F);
	assert(adapter.device_version == 0x00020000);
	assert((adapter.supported_features & (1u << ENA_ADMIN_DEVICE_ATTRIBUTES)) != 0);
	assert((adapter.supported_features & (1u << ENA_ADMIN_MTU)) != 0);
	assert(adapter.attr_caps == 1);
	assert(adapter.phys_addr_width == 48);
	assert(adapter.virt_addr_width == 48);
	assert(adapter.max_mtu == 1500);
	assert(adapter.mac_addr[0] == 0x52 && adapter.mac_addr[5] == 0x56);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_device_attributes passed\n");
}

static void test_init_queue_limits(void)
{
	printf("[TEST] Running test_init_queue_limits...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);

	assert(ena_init_get_queue_limits(&adapter) == 0);
	assert(adapter.max_tx_queues == 16);
	assert(adapter.max_rx_queues == 16);
	assert(adapter.max_tx_ring_size == 1024);
	assert(adapter.max_rx_ring_size == 1024);
	assert(adapter.max_header_size == 512);
	assert(adapter.max_packet_tx_descs == 8);
	assert(adapter.max_packet_rx_descs == 8);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_queue_limits passed\n");
}

static void test_init_host_info(void)
{
	printf("[TEST] Running test_init_host_info...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_init_get_device_attributes(&adapter) == 0);

	assert(ena_init_set_host_info(&adapter) == 0);
	assert(adapter.host_info_base != NULL);
	assert(adapter.host_info_phys != 0);

	/* The device must have been pointed at the same buffer. */
	assert(hw.host_info_base == adapter.host_info_base);
	assert(hw.host_info_debug_size == 0);

	const struct ena_admin_host_info *info = adapter.host_info_base;
	assert(info->os_type == ENA_ADMIN_OS_DPDK);
	assert(strcmp((const char *)info->os_dist_str, "Unikraft") == 0);
	assert(strstr((const char *)info->kernel_ver_str, "ena-unikraft") != NULL);
	assert((info->driver_version & 0xFFu) ==
	       ENA_INIT_DRIVER_VERSION_MAJOR);
	assert(info->ena_spec_version == ENA_INIT_ENA_SPEC_VERSION);
	assert(info->num_cpus == 1);

	ena_admin_fini(&adapter);
	assert(adapter.host_info_base == NULL);
	assert(adapter.host_info_phys == 0);
	printf("[PASS] test_init_host_info passed\n");
}

static void test_init_mtu_success(void)
{
	printf("[TEST] Running test_init_mtu_success...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_init_get_device_attributes(&adapter) == 0);

	assert(ena_init_set_mtu(&adapter, 1500) == 0);
	assert(hw.negotiated_mtu == 1500);
	assert(adapter.mtu == 1500);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_mtu_success passed\n");
}

static void test_init_mtu_invalid(void)
{
	printf("[TEST] Running test_init_mtu_invalid...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_init_get_device_attributes(&adapter) == 0);

	/* Below the minimum: the driver rejects it before sending. */
	assert(ena_init_set_mtu(&adapter, 10) == -EINVAL);
	/* Above the device maximum: the device rejects it. */
	assert(ena_init_set_mtu(&adapter, 9001) == -ENA_ADMIN_ILLEGAL_PARAMETER);

	/* Nothing was accepted. */
	assert(hw.negotiated_mtu == 0);
	assert(adapter.mtu == 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_mtu_invalid passed\n");
}

static void test_init_mac_roundtrip(void)
{
	printf("[TEST] Running test_init_mac_roundtrip...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	uint8_t mac[6];

	assert(setup_adapter(&hw, &adapter) == 0);
	memset(mac, 0, sizeof(mac));

	assert(ena_init_get_mac_addr(&adapter, mac) == 0);
	assert(mac[0] == 0x52 && mac[1] == 0x54 && mac[2] == 0x00);
	assert(mac[3] == 0x12 && mac[4] == 0x34 && mac[5] == 0x56);
	assert(memcmp(mac, adapter.mac_addr, 6) == 0);
	assert(memcmp(mac, hw.dev_mac, 6) == 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_mac_roundtrip passed\n");
}

static void test_init_ordering(void)
{
	printf("[TEST] Running test_init_ordering...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);

	/* The device rejects configuration before the attribute query. */
	mock_ena_hw_require_attrs_first(&hw, 1);
	assert(ena_init_set_mtu(&adapter, 1500) == -ENA_ADMIN_ILLEGAL_PARAMETER);
	assert(ena_init_set_host_info(&adapter) == -ENA_ADMIN_ILLEGAL_PARAMETER);
	assert(hw.negotiated_mtu == 0);
	assert(hw.host_info_base == NULL);
	assert(adapter.host_info_base == NULL);

	/* After the attribute query, the same commands succeed. */
	assert(ena_init_get_device_attributes(&adapter) == 0);
	assert(ena_init_set_mtu(&adapter, 1500) == 0);
	assert(ena_init_set_host_info(&adapter) == 0);
	assert(hw.negotiated_mtu == 1500);
	assert(hw.host_info_base != NULL);

	ena_admin_fini(&adapter);
	assert(adapter.host_info_base == NULL);
	printf("[PASS] test_init_ordering passed\n");
}

static void test_init_error_paths(void)
{
	printf("[TEST] Running test_init_error_paths...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	uint8_t mac[6];

	assert(ena_init_get_device_attributes(NULL) == -EINVAL);
	assert(ena_init_set_host_info(NULL) == -EINVAL);
	assert(ena_init_set_mtu(NULL, 1500) == -EINVAL);
	assert(ena_init_get_mac_addr(NULL, mac) == -EINVAL);
	assert(ena_init_run(NULL, 1500) == -EINVAL);

	assert(setup_adapter(&hw, &adapter) == 0);

	/* Unknown feature id: the device rejects it. */
	{
		struct ena_admin_get_feat_inline req;
		uint32_t resp[14];

		memset(&req, 0, sizeof(req));
		req.feat_common.feature_id = 99;
		assert(ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					 &req, sizeof(req), resp, sizeof(resp),
					 NULL, 100) == -ENA_ADMIN_ILLEGAL_PARAMETER);
	}

	/* Hung device: the command times out and transitions adapter to error state. */
	mock_ena_hw_hang_admin(&hw);
	assert(ena_init_set_mtu(&adapter, 1500) == -ETIMEDOUT);
	assert(adapter.state == ENA_STATE_ERROR);
	assert(ena_init_set_host_info(&adapter) == -ENODEV);
	mock_ena_hw_clear_admin_hang(&hw);

	/* Reinitialize adapter to test device error status propagation. */
	ena_admin_fini(&adapter);
	assert(setup_adapter(&hw, &adapter) == 0);
	mock_ena_hw_set_admin_status(&hw, ENA_ADMIN_ILLEGAL_PARAMETER);
	assert(ena_init_get_device_attributes(&adapter) == -5);
	mock_ena_hw_set_admin_status(&hw, 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_error_paths passed\n");
}

static void test_init_feat_select_flag(void)
{
	printf("[TEST] Running test_init_feat_select_flag...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);

	/* GET_FEATURE: the select field must request the current value. */
	assert(ena_init_get_device_attributes(&adapter) == 0);
	assert(hw.last_feat_flags != 0);
	assert((hw.last_feat_flags & 0x3u) == ENA_ADMIN_FEAT_SELECT_CURRENT);

	/* SET_FEATURE: the same encoding must be used. */
	assert(ena_init_set_mtu(&adapter, 1500) == 0);
	assert((hw.last_feat_flags & 0x3u) == ENA_ADMIN_FEAT_SELECT_CURRENT);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_feat_select_flag passed\n");
}

static void test_init_run_sequence(void)
{
	printf("[TEST] Running test_init_run_sequence...\n");

	struct mock_ena_hw hw;
	struct ena_adapter adapter;
	assert(setup_adapter(&hw, &adapter) == 0);

	assert(ena_init_run(&adapter, 1500) == 0);
	assert(adapter.state == ENA_STATE_CONFIGURED);
	assert(hw.attrs_read == 1);
	assert(hw.negotiated_mtu == 1500);
	assert(hw.host_info_base == adapter.host_info_base);
	assert(adapter.max_tx_queues == 16);
	assert(adapter.max_rx_ring_size == 1024);
	assert(adapter.mac_addr[0] == 0x52 && adapter.mac_addr[5] == 0x56);
	assert(adapter.mtu == 1500);
	assert(adapter.max_mtu == 1500);

	/* A run with an invalid MTU stops before completion. */
	ena_admin_fini(&adapter);
	assert(adapter.host_info_base == NULL);
	memset(&adapter, 0, sizeof(adapter));
	assert(setup_adapter(&hw, &adapter) == 0);
	assert(ena_init_run(&adapter, 9001) == -ENA_ADMIN_ILLEGAL_PARAMETER);
	assert(adapter.state == ENA_STATE_ADMIN_READY);

	ena_admin_fini(&adapter);
	printf("[PASS] test_init_run_sequence passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 3 Test Suite \n");
	printf("========================================\n");
	test_init_wire_layouts();
	test_init_device_attributes();
	test_init_queue_limits();
	test_init_host_info();
	test_init_mtu_success();
	test_init_mtu_invalid();
	test_init_mac_roundtrip();
	test_init_ordering();
	test_init_error_paths();
	test_init_feat_select_flag();
	test_init_run_sequence();
	printf("========================================\n");
	printf("ALL PHASE 3 INIT TESTS PASSED (11/11)   \n");
	printf("========================================\n");
	return 0;
}
