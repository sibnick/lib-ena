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

static void test_pci_id_matching(void)
{
	printf("[TEST] Running test_pci_id_matching...\n");
	assert(ena_pci_match_id(0x1D0F, 0x0EC2) == 1);
	assert(ena_pci_match_id(0x1D0F, 0x1EC2) == 1);
	assert(ena_pci_match_id(0x1D0F, 0xEC20) == 1);
	assert(ena_pci_match_id(0x1D0F, 0xEC21) == 1);
	assert(ena_pci_match_id(0x1D0F, 0x0051) == 1);

	/* Invalid IDs must not match. */
	assert(ena_pci_match_id(0x1D0F, 0xFFFF) == 0);
	assert(ena_pci_match_id(0x8086, 0x0EC2) == 0);
	printf("[PASS] test_pci_id_matching passed\n");
}

static void test_bar0_scaffold_initialization(void)
{
	printf("[TEST] Running test_bar0_scaffold_initialization...\n");
	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);

	struct ena_adapter adapter;
	int ret = ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ret == 0);
	assert(adapter.state == ENA_STATE_PCI_PROBED);
	assert(adapter.version == ((2 << 8) | 0));
	assert(adapter.controller_version == 0x00020800);
	assert(adapter.caps == 0x00040000);
	printf("[PASS] test_bar0_scaffold_initialization passed\n");
}

static void test_device_status_and_reset(void)
{
	printf("[TEST] Running test_device_status_and_reset...\n");
	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));

	assert(ena_device_check_ready(&adapter) == 0);

	/* Request a controller reset (writes DEV_CTL at 0x54). */
	int ret = ena_device_reset(&adapter);
	assert(ret == 0);

	/* Verify the reset request reached DEV_CTL. */
	uint32_t ctl_val = mock_ena_hw_get_reg32(&hw, ENA_REGS_DEV_CTL_OFF);
	assert(ctl_val & ENA_DEV_CTL_DEV_RESET_MASK);

	/* Device finishes the reset; the driver then confirms it (status at 0x58). */
	mock_ena_hw_trigger_reset_completion(&hw);
	ret = ena_device_wait_reset_complete(&adapter, 100);
	assert(ret == 0);

	/* Device must be ready again after the reset. */
	assert(ena_device_check_ready(&adapter) == 0);
	printf("[PASS] test_device_status_and_reset passed\n");
}

static void test_reset_delay_polls(void)
{
	printf("[TEST] Running test_reset_delay_polls...\n");
	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);

	struct ena_adapter adapter;
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);

	/* Model a reset that stays in progress for 50 polls, then finishes.
	 * A loop that gives up early would time out before poll 50. */
	mock_ena_hw_set_reg32(&hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_RESET_IN_PROG_MASK);
	hw.reset_polls_to_finish = 50;
	ena_device_set_reset_poll_hook(mock_ena_hw_reset_poll_hook, &hw);

	int ret = ena_device_wait_reset_complete(&adapter, 100);
	assert(ret == 0);
	assert(hw.reset_polls == 50);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_DEV_STS_OFF) &
	       ENA_DEV_STS_RESET_FIN_MASK);

	ena_device_set_reset_poll_hook(NULL, NULL);
	printf("[PASS] test_reset_delay_polls passed\n");
}

static void test_reset_in_progress_bit(void)
{
	printf("[TEST] Running test_reset_in_progress_bit...\n");
	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);

	struct ena_adapter adapter;
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);

	/* FINISHED is set but IN_PROGRESS is still set: not complete.
	 * The loop must keep polling and time out. */
	mock_ena_hw_set_reg32(&hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_RESET_FIN_MASK |
			      ENA_DEV_STS_RESET_IN_PROG_MASK);
	int ret = ena_device_wait_reset_complete(&adapter, 10);
	assert(ret == -ETIMEDOUT);

	/* Clear IN_PROGRESS: the reset is now complete. */
	mock_ena_hw_set_reg32(&hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_RESET_FIN_MASK |
			      ENA_DEV_STS_READY_MASK);
	ret = ena_device_wait_reset_complete(&adapter, 10);
	assert(ret == 0);

	printf("[PASS] test_reset_in_progress_bit passed\n");
}

static void test_reset_fatal_error(void)
{
	printf("[TEST] Running test_reset_fatal_error...\n");
	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);

	struct ena_adapter adapter;
	assert(ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0)) == 0);

	/* A fatal error during reset returns -EIO and sets the error state. */
	mock_ena_hw_set_reg32(&hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_FATAL_ERROR_MASK);
	int ret = ena_device_wait_reset_complete(&adapter, 10);
	assert(ret == -EIO);
	assert(adapter.state == ENA_STATE_ERROR);

	printf("[PASS] test_reset_fatal_error passed\n");
}

static void test_plat_dma_identity(void)
{
	printf("[TEST] Running test_plat_dma_identity...\n");
	uint64_t phys;
	void *buf = ena_dma_alloc(16, &phys);
	assert(buf != NULL);
	/* The host build uses identity mapping: phys == virt. */
	assert(phys == (uint64_t)(uintptr_t)buf);

	/* A NULL phys_out must not fault and must still allocate. */
	void *buf2 = ena_dma_alloc(4, NULL);
	assert(buf2 != NULL);

	/* Exercise the (light) calibrated delay; it must return promptly. */
	ena_delay_us(1);

	ena_dma_free(buf, phys);
	ena_dma_free(buf2, 0);
	printf("[PASS] test_plat_dma_identity passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 1 Test Suite \n");
	printf("========================================\n");
	test_pci_id_matching();
	test_bar0_scaffold_initialization();
	test_device_status_and_reset();
	test_reset_delay_polls();
	test_reset_in_progress_bit();
	test_reset_fatal_error();
	test_plat_dma_identity();
	printf("========================================\n");
	printf("ALL PHASE 1 SCAFFOLD TESTS PASSED (7/7) \n");
	printf("========================================\n");
	return 0;
}
