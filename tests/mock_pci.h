/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef _MOCK_PCI_H_
#define _MOCK_PCI_H_

#include <stdint.h>
#include <stddef.h>

#define MOCK_BAR0_SIZE 0x1000
#define MOCK_MAX_IO_QUEUES 32

enum mock_pci_fault_type {
	MOCK_PCI_FAULT_NONE = 0,
	MOCK_PCI_FAULT_BAD_DB_OFFSET,
	MOCK_PCI_FAULT_BAD_CMD_ID,
	MOCK_PCI_FAULT_FAKE_REQ_ID,
	MOCK_PCI_FAULT_CORRUPT_LENGTH,
	MOCK_PCI_FAULT_ADMIN_HANG,
	MOCK_PCI_FAULT_ADMIN_STATUS,
	MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET,
};

struct mock_io_cq_state {
	uint16_t cq_tail;
	uint8_t cq_phase;
};

struct mock_ena_hw {
	uint8_t bar0[MOCK_BAR0_SIZE];
	uint32_t reset_count;

	/* Reset wait test controls */
	uint32_t reset_polls;
	uint32_t reset_polls_to_finish;

	/* Phase 2: emulated admin device state */
	uint8_t *dev_aq_base;
	uint8_t *dev_acq_base;
	uint8_t *dev_aenq_base;
	uint16_t dev_aq_depth;
	uint16_t dev_acq_depth;
	uint16_t dev_aenq_depth;
	uint16_t dev_acq_tail;
	uint8_t dev_acq_phase;
	uint16_t dev_aenq_tail;
	uint8_t dev_aenq_phase;
	uint32_t dev_aenq_seq;

	/* Driver-side ACQ consumption, tracked from the ACQ tail register */
	uint16_t drv_acq_head;
	uint16_t last_acq_tail_reg;

	/* Test controls */
	uint8_t admin_status;
	uint8_t admin_hang;
	uint8_t inject_bad_cmd_id;
	uint16_t bad_cmd_id;

	/* Security audit fault injection controls */
	uint8_t inject_bad_db_offset;
	uint32_t bad_db_offset;

	uint8_t inject_fake_req_id;
	uint16_t fake_req_id;

	uint8_t inject_corrupt_len;
	uint16_t corrupt_len;

	/* Last consumed admin command for test assertions */
	uint8_t last_opcode;
	uint16_t last_command_id;

	/* Phase 3: emulated device attributes */
	uint32_t dev_impl_id;
	uint32_t dev_device_version;
	uint32_t dev_supported_features;
	uint32_t dev_capabilities;
	uint32_t dev_phys_addr_width;
	uint32_t dev_virt_addr_width;
	uint8_t dev_mac[6];
	uint32_t dev_max_mtu;
	uint32_t dev_max_sq_num;
	uint32_t dev_max_sq_depth;
	uint32_t dev_max_cq_num;
	uint32_t dev_max_cq_depth;

	/* Phase 3: records of accepted settings for assertions */
	uint8_t attrs_read;
	uint32_t negotiated_mtu;
	uint8_t *host_info_base;
	uint32_t host_info_debug_size;

	/* Phase 3: flags byte of the last get/set feature descriptor */
	uint8_t last_feat_flags;

	/* Phase 3: test controls */
	uint8_t require_attrs_first;

	/* Phase 4: IO queue emulation */
	uint16_t next_sq_id;
	uint16_t next_cq_id;
	uint32_t last_sq_phys;
	uint32_t last_cq_phys;
	uint16_t last_sq_depth;
	uint16_t last_cq_depth;
	uint8_t last_sq_direction;
	uint16_t last_sq_cq_idx;
	uint32_t cq_created_count;
	uint32_t sq_created_count;
	uint32_t cq_destroyed_count;
	uint32_t sq_destroyed_count;

	/* Phase 9: LLQ BAR2 emulation (0 = device without a BAR2) */
	uint32_t dev_llq_bar_size;
	uint32_t llq_next_off;
	uint8_t last_sq_placement;

	/* Phase 4: Mock CQ internal state tracking per queue */
	struct mock_io_cq_state io_tx_cq_state[MOCK_MAX_IO_QUEUES];
	struct mock_io_cq_state io_rx_cq_state[MOCK_MAX_IO_QUEUES];
};

void mock_ena_hw_init(struct mock_ena_hw *hw);
void mock_ena_hw_set_reg32(struct mock_ena_hw *hw, uint32_t offset, uint32_t value);
uint32_t mock_ena_hw_get_reg32(const struct mock_ena_hw *hw, uint32_t offset);
void mock_ena_hw_trigger_reset_completion(struct mock_ena_hw *hw);

/* Reset wait test hook */
void mock_ena_hw_reset_poll_hook(void *cookie);

/* Device-side admin queue and AENQ emulation */
void mock_ena_hw_aq_doorbell_hook(void *cookie, uint16_t tail);
uint16_t mock_ena_hw_settle_acq_head(struct mock_ena_hw *hw);
void mock_ena_hw_set_admin_status(struct mock_ena_hw *hw, uint8_t status);
void mock_ena_hw_hang_admin(struct mock_ena_hw *hw);
void mock_ena_hw_clear_admin_hang(struct mock_ena_hw *hw);
void mock_ena_hw_inject_bad_cmd_id(struct mock_ena_hw *hw, uint16_t id);
void mock_ena_hw_clear_bad_cmd_id(struct mock_ena_hw *hw);
void mock_ena_hw_inject_bad_db_offset(struct mock_ena_hw *hw, uint32_t offset);
void mock_ena_hw_clear_bad_db_offset(struct mock_ena_hw *hw);
void mock_ena_hw_inject_fake_req_id(struct mock_ena_hw *hw, uint16_t id);
void mock_ena_hw_clear_fake_req_id(struct mock_ena_hw *hw);
void mock_ena_hw_inject_aenq(struct mock_ena_hw *hw, uint16_t group, uint16_t syndrome);
void mock_ena_hw_inject_aenq_payload(struct mock_ena_hw *hw, uint16_t group,
				     uint16_t syndrome, uint32_t inline0);

/* Generic fault injection API */
void mock_pci_inject_fault(struct mock_ena_hw *hw, enum mock_pci_fault_type type, uint64_t arg);
void mock_pci_clear_faults(struct mock_ena_hw *hw);

/* Device-side feature negotiation */
void mock_ena_hw_require_attrs_first(struct mock_ena_hw *hw, int on);

/* TX packet processing and completion emulation */
struct ena_ring;
void mock_ena_hw_emulate_tx(struct mock_ena_hw *hw, struct ena_ring *ring, unsigned int count);

/* RX packet reception and completion emulation */
void mock_ena_hw_emulate_rx(struct mock_ena_hw *hw, struct ena_ring *ring,
			    unsigned int count, uint16_t pkt_len, uint32_t hash,
			    uint32_t status_flags);

#endif /* _MOCK_PCI_H_ */
