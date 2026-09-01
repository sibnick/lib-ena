/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "mock_pci.h"

#include <assert.h>
#include <errno.h>
#ifndef __Unikraft__
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* AENQ handler that records the events it is dispatched. */
struct aenq_log {
	int count;
	uint16_t groups[8];
	uint16_t syndromes[8];
};

static int test_aenq_handler(void *arg, uint16_t group, uint16_t syndrome,
			     const struct ena_admin_aenq_entry *entry)
{
	struct aenq_log *log = (struct aenq_log *)arg;

	(void)entry;
	if (log->count < 8) {
		log->groups[log->count] = group;
		log->syndromes[log->count] = syndrome;
	}
	log->count++;
	return 0;
}

static void test_admin_init_success(void)
{
	printf("[TEST] Running test_admin_init_success...\n");

	/* Wire layout must match the 64-byte ENA entry size. */
	assert(sizeof(struct ena_admin_aq_entry) == 64);
	assert(sizeof(struct ena_admin_acq_entry) == 64);
	assert(sizeof(struct ena_admin_aenq_entry) == 64);

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));

	int ret = ena_admin_init(&adapter, 8, 8, 8);
	assert(ret == 0);
	assert(adapter.state == ENA_STATE_ADMIN_READY);
	assert(adapter.aq_base != NULL);
	assert(adapter.acq_base != NULL);
	assert(adapter.aenq_base != NULL);
	assert(adapter.aq_depth == 8);
	assert(adapter.acq_depth == 8);
	assert(adapter.aenq_depth == 8);
	assert(adapter.acq_phase == 1);
	assert(adapter.aenq_phase == 1);

	/* BAR0 base registers must hold the (identity) physical base. */
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_AQ_BASE_LO_OFF) ==
	       (uint32_t)(uintptr_t)adapter.aq_base);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_AQ_BASE_HI_OFF) ==
	       (uint32_t)(((uint64_t)(uintptr_t)adapter.aq_base) >> 32));
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_ACQ_BASE_LO_OFF) ==
	       (uint32_t)(uintptr_t)adapter.acq_base);
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_AENQ_BASE_LO_OFF) ==
	       (uint32_t)(uintptr_t)adapter.aenq_base);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_init_success passed\n");
}

static void test_admin_init_bad_depth(void)
{
	printf("[TEST] Running test_admin_init_bad_depth...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));

	/* 6 is not a power of two. */
	assert(ena_admin_init(&adapter, 6, 8, 8) == -EINVAL);
	/* 3 is below the minimum depth of 4. */
	assert(ena_admin_init(&adapter, 8, 8, 3) == -EINVAL);
	/* 0 is invalid. */
	assert(ena_admin_init(&adapter, 0, 8, 8) == -EINVAL);
	/* No ring must have been left allocated. */
	assert(adapter.aq_base == NULL);
	assert(adapter.acq_base == NULL);
	assert(adapter.aenq_base == NULL);
	printf("[PASS] test_admin_init_bad_depth passed\n");
}

static void test_admin_cmd_roundtrip(void)
{
	printf("[TEST] Running test_admin_cmd_roundtrip...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	uint16_t command_id = 0xFFFF;
	uint32_t resp[14];
	memset(resp, 0, sizeof(resp));

	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				     resp, sizeof(resp), &command_id, 100);
	assert(ret == 0);
	assert(command_id == 1);
	assert(hw.last_opcode == ENA_ADMIN_GET_FEATURE);
	assert(hw.last_command_id == 1);
	/* The device fills the response with a known pattern. */
	assert(resp[0] == 0x5E5E0000u);
	assert(resp[13] == 0x5E5E000Du);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_cmd_roundtrip passed\n");
}

static void test_admin_cmd_error_status(void)
{
	printf("[TEST] Running test_admin_cmd_error_status...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	mock_ena_hw_set_admin_status(&hw, 5);

	uint16_t command_id = 0;
	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				     NULL, 0, &command_id, 100);
	assert(ret == -5);
	assert(command_id == 1);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_cmd_error_status passed\n");
}

static void test_admin_cmd_timeout(void)
{
	printf("[TEST] Running test_admin_cmd_timeout...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	mock_ena_hw_hang_admin(&hw);

	uint16_t command_id = 0;
	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				     NULL, 0, &command_id, 50);
	assert(ret == -ETIMEDOUT);

	/* After timeout and reset, adapter is in error state */
	assert(adapter.state == ENA_STATE_ERROR);
	ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				 NULL, 0, &command_id, 100);
	assert(ret == -ENODEV);

	/* Clear the hang and re-initialize admin queue; the next command must succeed. */
	mock_ena_hw_clear_admin_hang(&hw);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				 NULL, 0, &command_id, 100);
	assert(ret == 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_cmd_timeout passed\n");
}

static void test_admin_timeout_invalidates_io_queues(void)
{
	printf("[TEST] Running test_admin_timeout_invalidates_io_queues...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);
	ena_device_set_reset_poll_hook(mock_ena_hw_reset_poll_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	assert(ena_init_run(&adapter, 1500) == 0);

	/* Attach one TX and one RX ring so the reset has queues to invalidate */
	struct ena_ring *tx_ring = NULL;
	struct ena_ring *rx_ring = NULL;
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_TX, 8, 8, &tx_ring) == 0);
	assert(ena_ring_alloc(&adapter, 0, ENA_RING_TYPE_RX, 8, 8, &rx_ring) == 0);
	assert(ena_ring_create_hw(tx_ring, 0) == 0);
	assert(ena_ring_create_hw(rx_ring, 0) == 0);
	assert(tx_ring->hw_valid == true);
	assert(rx_ring->hw_valid == true);

	adapter.tx_rings = calloc(1, sizeof(struct ena_ring *));
	adapter.rx_rings = calloc(1, sizeof(struct ena_ring *));
	adapter.tx_rings[0] = tx_ring;
	adapter.num_tx_rings = 1;
	adapter.rx_rings[0] = rx_ring;
	adapter.num_rx_rings = 1;

	/* Leave a request in flight so invalidation has state to clear */
	struct ena_tx_pkt pkt;
	char pkt_data[32];
	uint16_t req_id = 0;

	memset(&pkt, 0, sizeof(pkt));
	pkt.len = sizeof(pkt_data);
	pkt.phys_addr = 0x50001000;
	assert(ena_tx_submit(tx_ring, &pkt, &req_id) == 0);
	assert(tx_ring->req_in_flight[req_id] == 1);


	/* Hang the admin queue: the command times out and the driver issues a
	 * device reset. The mock does not complete the reset, so the adapter
	 * stays in the error state. */
	mock_ena_hw_hang_admin(&hw);
	uint16_t cmd_id = 0;
	assert(ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				  NULL, 0, &cmd_id, 50) == -ETIMEDOUT);
	mock_ena_hw_clear_admin_hang(&hw);

	/* The reset destroyed the IO queues. The driver-side rings are
	 * invalid, the request pool is re-armed, and indices are zero. */
	assert(adapter.state == ENA_STATE_ERROR);
	assert(tx_ring->hw_valid == false);
	assert(rx_ring->hw_valid == false);
	assert(tx_ring->free_req_count == 8);
	assert(tx_ring->req_in_flight[req_id] == 0);
	assert(tx_ring->sq_tail == 0);
	assert(tx_ring->cq_head == 0);
	assert(tx_ring->sq_db == NULL);

	/* The data path returns a clean error instead of touching dead
	 * hardware: no submit, no completion consumed. The doorbell pointer
	 * is NULL, so ena_tx_doorbell cannot write to a dead queue. */
	assert(ena_tx_submit(tx_ring, &pkt, &req_id) == -ENODEV);
	assert(ena_rx_submit_one(rx_ring, pkt_data, 0x50001000,
				 sizeof(pkt_data), NULL) == -ENODEV);
	ena_tx_doorbell(tx_ring);
	assert(ena_tx_poll_completions(tx_ring, 8, NULL) == 0);
	{
		struct ena_rx_pkt pkts[4];
		assert(ena_rx_poll(rx_ring, pkts, 4) == 0);
	}

	/* Recovery: the reset completes (the device re-initializes its
	 * admin queue state), the admin queue is re-initialized, and the
	 * queues are re-created. The rings become valid again and
	 * transmit works. */
	hw.reset_polls_to_finish = 2;
	assert(ena_device_wait_reset_complete(&adapter, 100) == 0);
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	assert(adapter.state == ENA_STATE_ADMIN_READY);
	assert(ena_ring_create_hw(tx_ring, 0) == 0);
	assert(ena_ring_create_hw(rx_ring, 0) == 0);
	assert(tx_ring->hw_valid == true);
	assert(rx_ring->hw_valid == true);
	req_id = 0;
	assert(ena_tx_submit(tx_ring, &pkt, &req_id) == 0);

	ena_ring_free(tx_ring);
	ena_ring_free(rx_ring);
	free(adapter.tx_rings);
	free(adapter.rx_rings);
	ena_admin_fini(&adapter);
	ena_device_set_reset_poll_hook(NULL, NULL);

	printf("[PASS] test_admin_timeout_invalidates_io_queues passed\n");
}

static void test_admin_acq_phase_flip(void)
{
	printf("[TEST] Running test_admin_acq_phase_flip...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	/* ACQ depth of 4 so a wrap happens within 5 commands. */
	assert(ena_admin_init(&adapter, 8, 4, 8) == 0);
	assert(adapter.acq_phase == 1);

	for (int i = 0; i < 5; i++) {
		uint16_t command_id = 0;
		int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					     NULL, 0, NULL, 0, &command_id,
					     100);
		assert(ret == 0);
	}

	/* One full wrap of the 4-deep ACQ flipped the phase to 0. */
	assert(adapter.acq_phase == 0);
	assert(hw.dev_acq_phase == 0);
	assert(adapter.acq_head == 5);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_acq_phase_flip passed\n");
}

static void test_admin_aenq_dispatch(void)
{
	printf("[TEST] Running test_admin_aenq_dispatch...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	struct aenq_log log;
	memset(&log, 0, sizeof(log));
	assert(ena_admin_aenq_register(&adapter, test_aenq_handler, &log) == 0);

	/* The device injects two events. */
	mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_LINK_CHANGE, 0);
	mock_ena_hw_inject_aenq(&hw, ENA_ADMIN_FATAL_ERROR, 1);

	int n = ena_admin_aenq_poll(&adapter, 16);
	assert(n == 2);
	assert(log.count == 2);
	assert(log.groups[0] == ENA_ADMIN_LINK_CHANGE);
	assert(log.syndromes[0] == 0);
	assert(log.groups[1] == ENA_ADMIN_FATAL_ERROR);
	assert(log.syndromes[1] == 1);

	/* Events are consumed: a second poll must not re-deliver them. */
	n = ena_admin_aenq_poll(&adapter, 16);
	assert(n == 0);
	assert(log.count == 2);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_aenq_dispatch passed\n");
}

static void test_admin_fini_clears(void)
{
	printf("[TEST] Running test_admin_fini_clears...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);
	assert(ena_admin_aenq_register(&adapter, test_aenq_handler, NULL) == 0);

	ena_admin_fini(&adapter);

	assert(adapter.state == ENA_STATE_STOPPED);
	assert(adapter.aq_base == NULL);
	assert(adapter.acq_base == NULL);
	assert(adapter.aenq_base == NULL);
	assert(adapter.aq_phys == 0);
	assert(adapter.acq_phys == 0);
	assert(adapter.aenq_phys == 0);
	assert(adapter.aenq_handler == NULL);
	assert(adapter.aenq_handler_arg == NULL);

	/* fini must be safe to call again. */
	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_fini_clears passed\n");
}

/* Phase 2 audit: CAPS registers must carry the 64-byte entry size. */
static void test_admin_caps_entry_size(void)
{
	printf("[TEST] Running test_admin_caps_entry_size...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	uint32_t aq_caps = mock_ena_hw_get_reg32(&hw, ENA_REGS_AQ_CAPS_OFF);
	uint32_t acq_caps = mock_ena_hw_get_reg32(&hw, ENA_REGS_ACQ_CAPS_OFF);
	uint32_t aenq_caps = mock_ena_hw_get_reg32(&hw, ENA_REGS_AENQ_CAPS_OFF);

	/* Bits 31:16 hold the entry size in bytes, bits 15:0 the depth. */
	assert((aq_caps & ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_MASK) ==
	       ((uint32_t)sizeof(struct ena_admin_aq_entry)
	        << ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT));
	assert((aq_caps & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK) == 8);

	assert((acq_caps & ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_MASK) ==
	       ((uint32_t)sizeof(struct ena_admin_acq_entry)
	        << ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT));
	assert((acq_caps & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK) == 8);

	assert((aenq_caps & ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_MASK) ==
	       ((uint32_t)sizeof(struct ena_admin_aenq_entry)
	        << ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT));
	assert((aenq_caps & ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK) == 8);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_caps_entry_size passed\n");
}

/* Phase 2 audit: the ACQ tail register (0x30) must be updated after
 * each consumed completion. */
static void test_admin_acq_tail_register(void)
{
	printf("[TEST] Running test_admin_acq_tail_register...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* The register starts at 0, and each expected index differs from
	 * the previous one, so each assertion proves a fresh write. */
	assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_ACQ_TAIL_OFF) == 0);

	for (int i = 1; i <= 8; i++) {
		uint16_t command_id = 0;

		assert(ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					 NULL, 0, NULL, 0, &command_id,
					 100) == 0);

		/* The driver publishes the next slot it will read. */
		assert(mock_ena_hw_get_reg32(&hw, ENA_REGS_ACQ_TAIL_OFF) == (uint32_t)i);
	}

	/* All 8 slots were released, so the device never saw a full ring. */
	assert(adapter.acq_head == 8);
	assert(mock_ena_hw_settle_acq_head(&hw) == 8);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_acq_tail_register passed\n");
}

/* Phase 2 audit: a completion with a mismatched command id is rejected. */
static void test_admin_cmd_id_mismatch(void)
{
	printf("[TEST] Running test_admin_cmd_id_mismatch...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* The device completes with a command id that does not match. */
	mock_ena_hw_inject_bad_cmd_id(&hw, 0x0AB1);

	uint16_t command_id = 0;
	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				     NULL, 0, &command_id, 100);
	assert(ret == -EIO);

	/* The stale slot is consumed so the ring stays in lockstep. */
	assert(adapter.acq_head == 1);
	assert(adapter.next_command_id == 2);

	/* The ring still works after the rejection. */
	mock_ena_hw_clear_bad_cmd_id(&hw);
	ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				 NULL, 0, &command_id, 100);
	assert(ret == 0);
	assert(command_id == 2);
	assert(adapter.acq_head == 2);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_cmd_id_mismatch passed\n");
}

/* Phase 2 audit: the command id must wrap inside the 12-bit space. */
static void test_admin_cmd_id_wrap(void)
{
	printf("[TEST] Running test_admin_cmd_id_wrap...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* 4096 commands: the 12-bit id space must wrap. */
	for (int i = 1; i <= 4096; i++) {
		int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					     NULL, 0, NULL, 0, NULL, 100);

		assert(ret == 0);
		/* The counter must never leave the 12-bit space. */
		assert(adapter.next_command_id <= ENA_ADMIN_COMMAND_ID_MASK);
		/* Command 4095 used the last id before the wrap. */
		if (i == 4095)
			assert(hw.last_command_id == 0x0FFF);
	}

	/* Command 4096 skipped reserved id 0 and wrapped to id 1; the next id is 2. */
	assert(hw.last_command_id == 1);
	assert(adapter.acq_head == 4096);
	assert(adapter.next_command_id == 2);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_cmd_id_wrap passed\n");
}

#ifndef __Unikraft__
struct lock_worker_arg {
	struct ena_adapter *adapter;
	uint16_t command_id;
	volatile int ret;
	volatile int done;
};

static void *lock_worker_fn(void *argp)
{
	struct lock_worker_arg *arg = (struct lock_worker_arg *)argp;

	arg->ret = ena_admin_exec_cmd(arg->adapter, ENA_ADMIN_GET_FEATURE,
				      NULL, 0, NULL, 0, &arg->command_id,
				      100);
	arg->done = 1;
	return NULL;
}

/* Phase 2 audit: a caller must block on the held admin lock, and the
 * lock must be released when the command finishes. */
static void test_admin_exec_locking(void)
{
	printf("[TEST] Running test_admin_exec_locking...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* The lock is free before and after a successful command. */
	assert(adapter.admin_lock == 0);
	uint16_t command_id = 0;
	assert(ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0,
				 NULL, 0, &command_id, 100) == 0);
	assert(command_id == 1);
	assert(adapter.admin_lock == 0);

	/* Hold the lock: a second caller must block, not corrupt state. */
	adapter.admin_lock = 1;

	struct lock_worker_arg arg = { &adapter, 0, -1, 0 };
	pthread_t thread;
	assert(pthread_create(&thread, NULL, lock_worker_fn, &arg) == 0);

	struct timespec nap = { 0, 200 * 1000 * 1000 }; /* 200 ms */
	assert(nanosleep(&nap, NULL) == 0);

	/* The worker is still blocked on the held lock. */
	assert(arg.done == 0);
	assert(arg.ret == -1);
	assert(adapter.aq_tail == 1);

	/* Release the lock: the worker must complete its command. */
	adapter.admin_lock = 0;
	assert(pthread_join(thread, NULL) == 0);
	assert(arg.done == 1);
	assert(arg.ret == 0);
	assert(arg.command_id == 2);
	assert(adapter.admin_lock == 0);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_exec_locking passed\n");
}

struct worker_arg {
	struct ena_adapter *adapter;
	int count;
	int ret;
};

static void *worker_fn(void *argp)
{
	struct worker_arg *arg = (struct worker_arg *)argp;

	arg->ret = 0;
	for (int i = 0; i < arg->count; i++) {
		int ret = ena_admin_exec_cmd(arg->adapter, ENA_ADMIN_GET_FEATURE,
					     NULL, 0, NULL, 0, NULL, 100);
		if (ret != 0) {
			arg->ret = ret;
			break;
		}
	}
	return NULL;
}

/* Phase 2 audit: concurrent callers must be serialized; no command may
 * be lost or duplicated and the ring state must stay consistent. */
static void test_admin_exec_concurrent(void)
{
	printf("[TEST] Running test_admin_exec_concurrent...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	enum { nthreads = 4, ncmds = 64 };
	pthread_t threads[nthreads];
	struct worker_arg args[nthreads];

	for (int i = 0; i < nthreads; i++) {
		args[i].adapter = &adapter;
		args[i].count = ncmds;
		args[i].ret = -1;
		assert(pthread_create(&threads[i], NULL, worker_fn, &args[i]) ==
		       0);
	}

	for (int i = 0; i < nthreads; i++)
		assert(pthread_join(threads[i], NULL) == 0);

	for (int i = 0; i < nthreads; i++)
		assert(args[i].ret == 0);

	/* All 256 commands completed, with no lost or double slot. */
	assert(adapter.acq_head == nthreads * ncmds);
	assert(hw.dev_acq_tail == nthreads * ncmds);
	assert(mock_ena_hw_settle_acq_head(&hw) == nthreads * ncmds);
	assert(adapter.next_command_id == nthreads * ncmds + 1);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_exec_concurrent passed\n");
}
#endif /* !__Unikraft__ */

static void test_admin_aq_phase_wrap(void)
{
	printf("[TEST] Running test_admin_aq_phase_wrap...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));

	/* Initialize with AQ depth of 4 to trigger wrap quickly */
	assert(ena_admin_init(&adapter, 4, 4, 4) == 0);
	assert(adapter.aq_phase == 1);

	/* Submit 5 commands to wrap AQ past 4 */
	for (int i = 0; i < 5; i++) {
		uint16_t command_id = 0;
		int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					     NULL, 0, NULL, 0, &command_id, 100);
		assert(ret == 0);
	}

	/* AQ wrapped once: aq_phase should now be 0 */
	assert(adapter.aq_tail == 5);
	assert(adapter.aq_phase == 0);

	/* Submit 4 more commands to wrap AQ again back to phase 1 */
	for (int i = 0; i < 4; i++) {
		uint16_t command_id = 0;
		int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE,
					     NULL, 0, NULL, 0, &command_id, 100);
		assert(ret == 0);
	}

	assert(adapter.aq_tail == 9);
	assert(adapter.aq_phase == 1);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_aq_phase_wrap passed\n");
}

static void test_admin_error_state_rejection(void)
{
	printf("[TEST] Running test_admin_error_state_rejection...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	/* Set adapter to error state */
	adapter.state = ENA_STATE_ERROR;

	uint16_t cmd_id;
	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, NULL, 0, NULL, 0, &cmd_id, 100);
	assert(ret == -ENODEV);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_error_state_rejection passed\n");
}

static void test_admin_full_64b_entry_payload(void)
{
	printf("[TEST] Running test_admin_full_64b_entry_payload...\n");

	struct mock_ena_hw hw;
	mock_ena_hw_init(&hw);
	ena_admin_set_db_hook(mock_ena_hw_aq_doorbell_hook, &hw);

	struct ena_adapter adapter;
	ena_device_init_scaffold(&adapter, hw.bar0, sizeof(hw.bar0));
	assert(ena_admin_init(&adapter, 8, 8, 8) == 0);

	struct ena_admin_aq_entry full_entry;
	memset(&full_entry, 0, sizeof(full_entry));
	full_entry.u.inline_data_w1[0] = 0x12345678;
	/* Set feature_id = 1 (DEVICE_ATTRIBUTES) in feat_common byte 1 */
	full_entry.inline_data_w4[0] = ((uint32_t)ENA_ADMIN_DEVICE_ATTRIBUTES << 8) | 0x1;

	uint16_t cmd_id;
	int ret = ena_admin_exec_cmd(&adapter, ENA_ADMIN_GET_FEATURE, &full_entry, sizeof(full_entry),
				     NULL, 0, &cmd_id, 100);
	assert(ret == 0);

	/* Verify entry in AQ memory preserved inline data words */
	const struct ena_admin_aq_entry *aq = (const struct ena_admin_aq_entry *)adapter.aq_base;
	assert(aq[0].u.inline_data_w1[0] == 0x12345678);
	assert(aq[0].inline_data_w4[0] == (((uint32_t)ENA_ADMIN_DEVICE_ATTRIBUTES << 8) | 0x1));
	assert(aq[0].aq_common_desc.opcode == ENA_ADMIN_GET_FEATURE);

	ena_admin_fini(&adapter);
	printf("[PASS] test_admin_full_64b_entry_payload passed\n");
}

int main(void)
{
	printf("========================================\n");
	printf("Running Unikraft ENA Phase 2 Test Suite \n");
	printf("========================================\n");
	test_admin_init_success();
	test_admin_init_bad_depth();
	test_admin_cmd_roundtrip();
	test_admin_cmd_error_status();
	test_admin_cmd_timeout();
	test_admin_timeout_invalidates_io_queues();
	test_admin_acq_phase_flip();
	test_admin_aenq_dispatch();
	test_admin_fini_clears();
	test_admin_caps_entry_size();
	test_admin_acq_tail_register();
	test_admin_cmd_id_mismatch();
	test_admin_cmd_id_wrap();
#ifndef __Unikraft__
	test_admin_exec_locking();
	test_admin_exec_concurrent();
#endif
	test_admin_aq_phase_wrap();
	test_admin_error_state_rejection();
	test_admin_full_64b_entry_payload();
	printf("========================================\n");
	printf("ALL PHASE 2 ADMIN TESTS PASSED\n");
	printf("========================================\n");
	return 0;
}
