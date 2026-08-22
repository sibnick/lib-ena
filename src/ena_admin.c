/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"

#include <errno.h>
#include <string.h>

#ifndef __Unikraft__
static ena_admin_db_hook *s_db_hook;
static void *s_db_cookie;

void ena_admin_set_db_hook(ena_admin_db_hook *hook, void *cookie)
{
	s_db_hook = hook;
	s_db_cookie = cookie;
}
#endif

static int ena_depth_ok(uint16_t depth)
{
	return (depth >= 4) && ((depth & (depth - 1)) == 0);
}

/* Serialize concurrent admin command execution.
 *
 * Design choice: a test-and-set busy flag built from the compiler atomic
 * builtins (__sync_lock_test_and_set / __sync_lock_release), with a
 * bounded spin while the flag is held. The same code runs on the host
 * test harness and under Unikraft, so no threading library is needed
 * (pthread is not available under Unikraft, and uk_spinlock would add a
 * platform dependency for a section that only ever lasts a bounded poll
 * loop). The flag lives in struct ena_adapter, so two different
 * adapters do not block each other.
 */
static void ena_admin_lock_take(uint32_t *lock)
{
	while (__sync_lock_test_and_set(lock, 1u) != 0u)
		ena_delay_us(1);
}

static void ena_admin_lock_drop(uint32_t *lock)
{
	__sync_lock_release(lock);
}

static void ena_admin_ring_aq_db(struct ena_adapter *adapter, uint16_t idx)
{
	ena_wmb();
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AQ_DB_OFF, idx);
	ena_mb();

#ifndef __Unikraft__
	if (s_db_hook)
		s_db_hook(s_db_cookie, idx);
#endif
}

int ena_admin_init(struct ena_adapter *adapter, uint16_t aq_depth,
		   uint16_t acq_depth, uint16_t aenq_depth)
{
	uint64_t aq_phys = 0, acq_phys = 0, aenq_phys = 0;
	void *aq, *acq, *aenq;

	if (!adapter || !adapter->bar0_base)
		return -EINVAL;

	if (adapter->aq_base || adapter->acq_base || adapter->aenq_base)
		ena_admin_fini(adapter);

	if (!ena_depth_ok(aq_depth) || !ena_depth_ok(acq_depth) ||
	    !ena_depth_ok(aenq_depth)) {
		ena_err("admin init: invalid ring depth");
		return -EINVAL;
	}

	aq = ena_dma_alloc((size_t)aq_depth * sizeof(struct ena_admin_aq_entry),
			   &aq_phys);
	if (!aq) {
		ena_err("admin init: AQ alloc failed");
		return -ENOMEM;
	}

	acq = ena_dma_alloc((size_t)acq_depth * sizeof(struct ena_admin_acq_entry),
			    &acq_phys);
	if (!acq) {
		ena_err("admin init: ACQ alloc failed");
		ena_dma_free(aq, aq_phys);
		return -ENOMEM;
	}

	aenq = ena_dma_alloc((size_t)aenq_depth * sizeof(struct ena_admin_aenq_entry),
			     &aenq_phys);
	if (!aenq) {
		ena_err("admin init: AENQ alloc failed");
		ena_dma_free(aq, aq_phys);
		ena_dma_free(acq, acq_phys);
		return -ENOMEM;
	}

	memset(aq, 0, (size_t)aq_depth * sizeof(struct ena_admin_aq_entry));
	memset(acq, 0, (size_t)acq_depth * sizeof(struct ena_admin_acq_entry));
	memset(aenq, 0, (size_t)aenq_depth * sizeof(struct ena_admin_aenq_entry));

	adapter->aq_base = aq;
	adapter->aq_phys = aq_phys;
	adapter->aq_depth = aq_depth;
	adapter->aq_tail = 0;
	adapter->aq_phase = 1;
	adapter->next_command_id = 1;

	adapter->acq_base = acq;
	adapter->acq_phys = acq_phys;
	adapter->acq_depth = acq_depth;
	adapter->acq_head = 0;
	adapter->acq_phase = 1;

	adapter->aenq_base = aenq;
	adapter->aenq_phys = aenq_phys;
	adapter->aenq_depth = aenq_depth;
	adapter->aenq_head = 0;
	adapter->aenq_phase = 1;

	adapter->aenq_handler = NULL;
	adapter->aenq_handler_arg = NULL;

	/* CAPS registers pack the queue depth in bits 15:0 and the entry
	 * size in bytes in bits 31:16. The entry size is derived from the
	 * actual wire descriptor structs. */
	uint32_t aq_caps = ((uint32_t)sizeof(struct ena_admin_aq_entry)
			    << ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT)
			   | (uint32_t)(aq_depth & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK);
	uint32_t acq_caps = ((uint32_t)sizeof(struct ena_admin_acq_entry)
			     << ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT)
			    | (uint32_t)(acq_depth & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK);
	uint32_t aenq_caps = ((uint32_t)sizeof(struct ena_admin_aenq_entry)
			      << ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT)
			     | (uint32_t)(aenq_depth & ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK);

	/* Publish the ring bases, depths, and entry sizes to the device. */
	ena_wmb();
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AQ_BASE_LO_OFF,
			(uint32_t)(aq_phys & 0xFFFFFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AQ_BASE_HI_OFF,
			(uint32_t)((aq_phys >> 32) & 0xFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AQ_CAPS_OFF, aq_caps);

	ena_reg_write32(adapter->bar0_base + ENA_REGS_ACQ_BASE_LO_OFF,
			(uint32_t)(acq_phys & 0xFFFFFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_ACQ_BASE_HI_OFF,
			(uint32_t)((acq_phys >> 32) & 0xFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_ACQ_CAPS_OFF, acq_caps);

	ena_reg_write32(adapter->bar0_base + ENA_REGS_AENQ_BASE_LO_OFF,
			(uint32_t)(aenq_phys & 0xFFFFFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AENQ_BASE_HI_OFF,
			(uint32_t)((aenq_phys >> 32) & 0xFFFFu));
	ena_reg_write32(adapter->bar0_base + ENA_REGS_AENQ_CAPS_OFF, aenq_caps);
	ena_mb();

	adapter->state = ENA_STATE_ADMIN_READY;
	ena_info("admin ready: aq=%u acq=%u aenq=%u", aq_depth, acq_depth,
		 aenq_depth);
	return 0;
}

void ena_admin_fini(struct ena_adapter *adapter)
{
	if (!adapter)
		return;

	if (adapter->aq_base) {
		ena_dma_free(adapter->aq_base, adapter->aq_phys);
		adapter->aq_base = NULL;
	}
	if (adapter->acq_base) {
		ena_dma_free(adapter->acq_base, adapter->acq_phys);
		adapter->acq_base = NULL;
	}
	if (adapter->aenq_base) {
		ena_dma_free(adapter->aenq_base, adapter->aenq_phys);
		adapter->aenq_base = NULL;
	}

	adapter->aq_phys = 0;
	adapter->acq_phys = 0;
	adapter->aenq_phys = 0;
	adapter->aq_depth = 0;
	adapter->acq_depth = 0;
	adapter->aenq_depth = 0;
	adapter->aq_tail = 0;
	adapter->aq_phase = 0;
	adapter->next_command_id = 0;
	adapter->acq_head = 0;
	adapter->acq_phase = 0;
	adapter->aenq_head = 0;
	adapter->aenq_phase = 0;
	adapter->aenq_handler = NULL;
	adapter->aenq_handler_arg = NULL;

	/* Free the Phase 3 host info buffer. */
	if (adapter->host_info_base) {
		ena_dma_free(adapter->host_info_base, adapter->host_info_phys);
		adapter->host_info_base = NULL;
	}
	adapter->host_info_phys = 0;

	adapter->state = ENA_STATE_STOPPED;
}

/* Submit one admin command and wait for its completion.
 * The caller must hold adapter->admin_lock. */
static int ena_admin_exec_locked(struct ena_adapter *adapter, uint8_t opcode,
				 const void *req, size_t req_len, void *resp,
				 size_t resp_cap, uint16_t *out_command_id,
				 unsigned int max_polls)
{
	uint16_t aq_mask;
	uint16_t acq_mask;
	uint16_t idx;
	uint16_t command_id;
	bool found = false;
	struct ena_admin_aq_entry *entry;
	struct ena_admin_acq_entry *acq;

	if (req && req_len > sizeof(struct ena_admin_aq_entry)) {
		ena_err("exec_cmd: request too large (%zu)", req_len);
		return -EINVAL;
	}

	if (max_polls == 0)
		max_polls = 1;

	aq_mask = adapter->aq_depth - 1;
	acq_mask = adapter->acq_depth - 1;

	/* Build the AQ entry. */
	idx = adapter->aq_tail & aq_mask;
	entry = (struct ena_admin_aq_entry *)adapter->aq_base + idx;

	/* Command ids occupy a 12-bit space; mask after each increment. */
	command_id = adapter->next_command_id;
	adapter->next_command_id =
		(uint16_t)((adapter->next_command_id + 1) &
			   ENA_ADMIN_COMMAND_ID_MASK);

	memset(entry, 0, sizeof(*entry));

	/* Copy the request payload. If caller passed full 64-byte command struct,
	 * copy starting from offset 0, otherwise copy into inline data area. */
	if (req && req_len > 0) {
		if (req_len >= sizeof(struct ena_admin_aq_entry))
			memcpy(entry, req, sizeof(struct ena_admin_aq_entry));
		else
			memcpy((uint8_t *)entry + sizeof(struct ena_admin_aq_common_desc),
			       req, req_len);
	}

	entry->aq_common_desc.command_id = command_id;
	entry->aq_common_desc.opcode = opcode;
	entry->aq_common_desc.flags = adapter->aq_phase & ENA_ADMIN_AQ_PHASE_MASK;

	/* Reserve the slot and ring the doorbell. */
	adapter->aq_tail++;
	if ((adapter->aq_tail & aq_mask) == 0)
		adapter->aq_phase ^= 1;

	ena_admin_ring_aq_db(adapter, idx);

	/* Poll the ACQ for the matching completion. */
	acq = (struct ena_admin_acq_entry *)adapter->acq_base +
	      (adapter->acq_head & acq_mask);

	for (unsigned int i = 0; i < max_polls; i++) {
		volatile const uint8_t *flags_ptr =
			(volatile const uint8_t *)&acq->acq_common_desc.flags;
		if ((*flags_ptr & ENA_ADMIN_ACQ_PHASE_MASK) == adapter->acq_phase) {
			ena_rmb();
			found = true;
			break;
		}
		ena_delay_us(1);
	}

	if (!found) {
		ena_err("exec_cmd: timeout after %u polls (resetting device)", max_polls);
		adapter->state = ENA_STATE_ERROR;
		ena_device_reset(adapter);
		return -ETIMEDOUT;
	}

	/* Consume the slot: advance the ACQ head, flip the phase on wrap,
	 * and release the slot to the device via the ACQ tail register. */
	adapter->acq_head++;
	if ((adapter->acq_head & acq_mask) == 0)
		adapter->acq_phase ^= 1;
	ena_wmb();
	ena_reg_write32(adapter->bar0_base + ENA_REGS_ACQ_TAIL_OFF,
			adapter->acq_head & acq_mask);
	ena_mb();

	/* Reject a stale completion that does not match the command id of
	 * the request we submitted. */
	if ((acq->acq_common_desc.command & ENA_ADMIN_COMMAND_ID_MASK) !=
	    command_id) {
		ena_err("exec_cmd: completion id %u does not match "
			"command id %u",
			(unsigned)(acq->acq_common_desc.command &
				   ENA_ADMIN_COMMAND_ID_MASK),
			(unsigned)command_id);
		return -EIO;
	}

	/* Capture the response. */
	if (out_command_id)
		*out_command_id =
			acq->acq_common_desc.command & ENA_ADMIN_COMMAND_ID_MASK;

	if (resp && resp_cap > 0) {
		size_t n = sizeof(acq->response_specific_data);

		if (n > resp_cap)
			n = resp_cap;
		memcpy(resp, acq->response_specific_data, n);
	}

	if (acq->acq_common_desc.status != 0) {
		ena_warn("exec_cmd: device status %u for command %u",
			 acq->acq_common_desc.status,
			 acq->acq_common_desc.command & ENA_ADMIN_COMMAND_ID_MASK);
		return -(int)acq->acq_common_desc.status;
	}

	return 0;
}

int ena_admin_exec_cmd(struct ena_adapter *adapter, uint8_t opcode,
		       const void *req, size_t req_len, void *resp,
		       size_t resp_cap, uint16_t *out_command_id,
		       unsigned int max_polls)
{
	int ret;

	if (!adapter || !adapter->aq_base || !adapter->acq_base)
		return -EINVAL;

	if (adapter->state == ENA_STATE_ERROR)
		return -ENODEV;

	/* Serialize the submit-and-wait section so concurrent control-plane
	 * callers cannot corrupt the AQ tail, ACQ head, and command id. */
	ena_admin_lock_take(&adapter->admin_lock);

	ret = ena_admin_exec_locked(adapter, opcode, req, req_len, resp,
				    resp_cap, out_command_id, max_polls);

	ena_admin_lock_drop(&adapter->admin_lock);
	return ret;
}

int ena_admin_aenq_register(struct ena_adapter *adapter,
			    ena_aenq_handler *handler, void *arg)
{
	if (!adapter || !handler)
		return -EINVAL;

	adapter->aenq_handler = handler;
	adapter->aenq_handler_arg = arg;
	return 0;
}

int ena_admin_aenq_poll(struct ena_adapter *adapter, unsigned int max_events)
{
	uint16_t aenq_mask;
	uint16_t idx;
	int dispatched = 0;
	struct ena_admin_aenq_entry *entry;

	if (!adapter || !adapter->aenq_base)
		return -EINVAL;

	aenq_mask = adapter->aenq_depth - 1;

	while ((unsigned int)dispatched < max_events) {
		volatile const uint8_t *flags_ptr;

		idx = adapter->aenq_head & aenq_mask;
		entry = (struct ena_admin_aenq_entry *)adapter->aenq_base + idx;
		flags_ptr = (volatile const uint8_t *)&entry->aenq_common_desc.flags;

		if ((*flags_ptr & ENA_ADMIN_AENQ_PHASE_MASK) !=
		    adapter->aenq_phase)
			break;

		ena_rmb();

		/* Advance the head and flip the phase on wrap. */
		adapter->aenq_head++;
		if ((adapter->aenq_head & aenq_mask) == 0)
			adapter->aenq_phase ^= 1;

		/* Acknowledge the event to the device. */
		ena_wmb();
		ena_reg_write32(adapter->bar0_base + ENA_REGS_AENQ_HEAD_DB_OFF,
				adapter->aenq_head & aenq_mask);
		ena_mb();

		/* Dispatch to the handler (if one is registered). */
		if (adapter->aenq_handler)
			adapter->aenq_handler(adapter->aenq_handler_arg,
					      entry->aenq_common_desc.group,
					      entry->aenq_common_desc.syndrome,
					      entry);

		dispatched++;
	}

	return dispatched;
}
