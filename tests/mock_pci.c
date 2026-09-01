/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "mock_pci.h"
#include "ena_regs.h"
#include "ena_admin.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_llq.h"

#include <stdint.h>
#include <string.h>

/* Rebuild a 48-bit physical address from its LO/HI register pair */
static uint64_t mock_read_phys(const struct mock_ena_hw *hw, uint32_t lo_off,
			       uint32_t hi_off)
{
	uint32_t lo = mock_ena_hw_get_reg32(hw, lo_off);
	uint32_t hi = mock_ena_hw_get_reg32(hw, hi_off);

	return (uint64_t)lo | ((uint64_t)hi << 32);
}

void mock_ena_hw_init(struct mock_ena_hw *hw)
{
	int i;

	if (!hw)
		return;

	memset(hw->bar0, 0, MOCK_BAR0_SIZE);
	hw->reset_count = 0;
	hw->reset_polls = 0;
	hw->reset_polls_to_finish = 0;

	/* Phase 2: reset the emulated admin device */
	hw->dev_aq_base = NULL;
	hw->dev_acq_base = NULL;
	hw->dev_aenq_base = NULL;
	hw->dev_aq_depth = 0;
	hw->dev_acq_depth = 0;
	hw->dev_aenq_depth = 0;
	hw->dev_acq_tail = 0;
	hw->dev_acq_phase = 1;
	hw->dev_aenq_tail = 0;
	hw->dev_aenq_phase = 1;
	hw->dev_aenq_seq = 0;
	hw->drv_acq_head = 0;
	hw->last_acq_tail_reg = 0;
	hw->admin_status = 0;
	hw->admin_hang = 0;
	hw->inject_bad_cmd_id = 0;
	hw->bad_cmd_id = 0;
	hw->inject_bad_db_offset = 0;
	hw->bad_db_offset = 0;
	hw->inject_fake_req_id = 0;
	hw->fake_req_id = 0;
	hw->inject_corrupt_len = 0;
	hw->corrupt_len = 0;
	hw->last_opcode = 0;
	hw->last_command_id = 0;

	/* Phase 3: default emulated device attributes */
	hw->dev_impl_id = 0x1D0F;
	hw->dev_device_version = 0x00020000;
	hw->dev_supported_features = (1u << ENA_ADMIN_DEVICE_ATTRIBUTES) |
				     (1u << ENA_ADMIN_MAX_QUEUES_NUM) |
				     (1u << ENA_ADMIN_MTU) |
				     (1u << ENA_ADMIN_HOST_ATTR_CONFIG) |
				     (1u << ENA_ADMIN_LLQ);
	hw->dev_capabilities = 1; /* ENA_ADMIN_ENI_STATS */
	hw->dev_phys_addr_width = 48;
	hw->dev_virt_addr_width = 48;
	hw->dev_mac[0] = 0x52;
	hw->dev_mac[1] = 0x54;
	hw->dev_mac[2] = 0x00;
	hw->dev_mac[3] = 0x12;
	hw->dev_mac[4] = 0x34;
	hw->dev_mac[5] = 0x56;
	hw->dev_max_mtu = 1500;
	hw->dev_max_sq_num = 16;
	hw->dev_max_sq_depth = 1024;
	hw->dev_max_cq_num = 16;
	hw->dev_max_cq_depth = 1024;

	/* Phase 3: negotiation records and controls */
	hw->attrs_read = 0;
	hw->negotiated_mtu = 0;
	hw->host_info_base = NULL;
	hw->host_info_debug_size = 0;
	hw->last_feat_flags = 0;
	hw->require_attrs_first = 0;

	/* Phase 4: IO queue emulation */
	hw->next_sq_id = 0;
	hw->next_cq_id = 0;
	hw->last_sq_phys = 0;
	hw->last_cq_phys = 0;
	hw->last_sq_depth = 0;
	hw->last_cq_depth = 0;
	hw->last_sq_direction = 0;
	hw->last_sq_cq_idx = 0;
	hw->cq_created_count = 0;
	hw->sq_created_count = 0;
	hw->cq_destroyed_count = 0;
	hw->sq_destroyed_count = 0;

	/* Phase 9: LLQ BAR2 emulation */
	hw->dev_llq_bar_size = 0;
	hw->llq_next_off = 0x1000;
	hw->last_sq_placement = 0;

	for (i = 0; i < MOCK_MAX_IO_QUEUES; i++) {
		hw->io_tx_cq_state[i].cq_tail = 0;
		hw->io_tx_cq_state[i].cq_phase = 1;
		hw->io_rx_cq_state[i].cq_tail = 0;
		hw->io_rx_cq_state[i].cq_phase = 1;
	}

	/* Default hardware version and controller values */
	mock_ena_hw_set_reg32(hw, ENA_REGS_VERSION_OFF, (2 << 8) | 0); /* v2.0 */
	mock_ena_hw_set_reg32(hw, ENA_REGS_CONTROLLER_VERSION_OFF, 0x00020800);
	mock_ena_hw_set_reg32(hw, ENA_REGS_CAPS_OFF, 0x00040000);
	mock_ena_hw_set_reg32(hw, ENA_REGS_DEV_STS_OFF, ENA_DEV_STS_READY_MASK);
}

void mock_ena_hw_set_reg32(struct mock_ena_hw *hw, uint32_t offset, uint32_t value)
{
	if (!hw || offset + sizeof(uint32_t) > MOCK_BAR0_SIZE)
		return;

	*(uint32_t *)(hw->bar0 + offset) = value;
}

uint32_t mock_ena_hw_get_reg32(const struct mock_ena_hw *hw, uint32_t offset)
{
	if (!hw || offset + sizeof(uint32_t) > MOCK_BAR0_SIZE)
		return 0;

	return *(const uint32_t *)(hw->bar0 + offset);
}

void mock_ena_hw_trigger_reset_completion(struct mock_ena_hw *hw)
{
	if (!hw)
		return;

	hw->reset_count++;
	mock_ena_hw_set_reg32(hw, ENA_REGS_DEV_STS_OFF,
			      ENA_DEV_STS_RESET_FIN_MASK | ENA_DEV_STS_READY_MASK);
}

void mock_ena_hw_reset_poll_hook(void *cookie)
{
	struct mock_ena_hw *hw = (struct mock_ena_hw *)cookie;

	if (!hw)
		return;

	/* Count this poll. Report reset finished when budget is reached */
	hw->reset_polls++;
	if (hw->reset_polls_to_finish != 0 &&
	    hw->reset_polls >= hw->reset_polls_to_finish) {
		mock_ena_hw_set_reg32(hw, ENA_REGS_DEV_STS_OFF,
				      ENA_DEV_STS_RESET_FIN_MASK |
				      ENA_DEV_STS_READY_MASK);
	}
}

/* Inline payload of a get/set feature command (60-byte AQ inline region) */
struct mock_get_set_feat_inline {
	uint32_t ctrl_len;
	uint32_t ctrl_lo;
	uint16_t ctrl_hi;
	uint16_t ctrl_res;
	uint8_t flags;
	uint8_t feature_id;
	uint8_t feature_version;
	uint8_t reserved8;
	uint32_t raw[11];
};

/* Emulate the device reaction to get/set feature commands */
static void mock_dispatch_feature(struct mock_ena_hw *hw,
				  const struct ena_admin_aq_entry *req,
				  struct ena_admin_acq_entry *comp)
{
	const struct mock_get_set_feat_inline *feat;
	uint8_t status = hw->admin_status;
	int filled = 0;
	int i;

	if (status == 0) {
		if (req->aq_common_desc.opcode == ENA_ADMIN_CREATE_CQ) {
			const struct ena_admin_aq_create_cq_cmd *cmd =
				(const struct ena_admin_aq_create_cq_cmd *)req;
			struct ena_admin_acq_create_cq_resp_desc *resp =
				(struct ena_admin_acq_create_cq_resp_desc *)comp;

			if (cmd->cq_depth == 0 || (cmd->cq_depth & (cmd->cq_depth - 1)) != 0 ||
			    cmd->cq_depth > hw->dev_max_cq_depth) {
				status = ENA_ADMIN_ILLEGAL_PARAMETER;
			} else {
				hw->cq_created_count++;
				hw->last_cq_depth = cmd->cq_depth;
				hw->last_cq_phys = cmd->cq_ba.mem_addr_low;
				resp->cq_idx = hw->next_cq_id++;
				resp->cq_actual_depth = cmd->cq_depth;
				resp->cq_head_db_register_offset =
					hw->inject_bad_db_offset ?
					hw->bad_db_offset : 0x30;
				resp->cq_interrupt_unmask_register_offset = 0x4C;
				filled = 1;
			}
		} else if (req->aq_common_desc.opcode == ENA_ADMIN_DESTROY_CQ) {
			hw->cq_destroyed_count++;
			filled = 1;
		} else if (req->aq_common_desc.opcode == ENA_ADMIN_CREATE_SQ) {
			const struct ena_admin_aq_create_sq_cmd *cmd =
				(const struct ena_admin_aq_create_sq_cmd *)req;
			struct ena_admin_acq_create_sq_resp_desc *resp =
				(struct ena_admin_acq_create_sq_resp_desc *)comp;

			if (cmd->sq_depth == 0 || (cmd->sq_depth & (cmd->sq_depth - 1)) != 0 ||
			    cmd->sq_depth > hw->dev_max_sq_depth) {
				status = ENA_ADMIN_ILLEGAL_PARAMETER;
			} else {
				uint8_t placement = (uint8_t)(cmd->sq_caps_2 & 0x0Fu);

				hw->sq_created_count++;
				hw->last_sq_depth = cmd->sq_depth;
				hw->last_sq_phys = cmd->sq_ba.mem_addr_low;
				hw->last_sq_direction = (cmd->sq_identity >> 5) & 0x7;
				hw->last_sq_cq_idx = cmd->cq_idx;
				hw->last_sq_placement = placement;
				resp->sq_idx = hw->next_sq_id++;
				resp->sq_doorbell_offset =
					hw->inject_bad_db_offset ?
					hw->bad_db_offset : 0x2C;

				/* Device-placement queues live in the LLQ
				 * BAR2. The response returns the BAR2
				 * offsets of the descriptor ring and the
				 * header ring. */
				if (placement == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
					size_t descs_area;
					size_t headers_area;

					if (!(hw->dev_supported_features &
					      (1u << ENA_ADMIN_LLQ)) ||
					    hw->dev_llq_bar_size == 0) {
						status = ENA_ADMIN_ILLEGAL_PARAMETER;
					} else {
						descs_area = (size_t)cmd->sq_depth * 128;
						headers_area = (size_t)cmd->sq_depth * 128;

						if ((size_t)hw->llq_next_off +
						    descs_area + headers_area >
						    hw->dev_llq_bar_size) {
							status = ENA_ADMIN_ILLEGAL_PARAMETER;
						} else {
							uint32_t descs_off = hw->llq_next_off;

							resp->llq_descriptors_offset = descs_off;
							resp->llq_headers_offset =
								descs_off + (uint32_t)descs_area;
							hw->llq_next_off +=
								(uint32_t)(descs_area +
									     headers_area);
						}
					}
				}

				filled = 1;
			}
		} else if (req->aq_common_desc.opcode == ENA_ADMIN_DESTROY_SQ) {
			hw->sq_destroyed_count++;
			filled = 1;
		} else if (req->aq_common_desc.opcode == ENA_ADMIN_GET_FEATURE ||
			   req->aq_common_desc.opcode == ENA_ADMIN_SET_FEATURE) {

		feat = (const struct mock_get_set_feat_inline *)
		       req->u.inline_data_w1;
		hw->last_feat_flags = feat->flags;

		if (req->aq_common_desc.opcode == ENA_ADMIN_GET_FEATURE) {
			switch (feat->feature_id) {
			case ENA_ADMIN_DEVICE_ATTRIBUTES: {
				struct ena_admin_device_attr_feature_desc *attr;

				attr = (struct ena_admin_device_attr_feature_desc *)
				       comp->response_specific_data;
				attr->impl_id = hw->dev_impl_id;
				attr->device_version = hw->dev_device_version;
				attr->supported_features =
					hw->dev_supported_features;
				attr->capabilities = hw->dev_capabilities;
				attr->phys_addr_width = hw->dev_phys_addr_width;
				attr->virt_addr_width = hw->dev_virt_addr_width;
				memcpy(attr->mac_addr, hw->dev_mac, 6);
				attr->reserved7[0] = 0;
				attr->reserved7[1] = 0;
				attr->max_mtu = hw->dev_max_mtu;
				hw->attrs_read = 1;
				filled = 1;
				break;
			}
			case ENA_ADMIN_MAX_QUEUES_NUM: {
				struct ena_admin_queue_feature_desc *q;

				q = (struct ena_admin_queue_feature_desc *)
				    comp->response_specific_data;
				q->max_sq_num = hw->dev_max_sq_num;
				q->max_sq_depth = hw->dev_max_sq_depth;
				q->max_cq_num = hw->dev_max_cq_num;
				q->max_cq_depth = hw->dev_max_cq_depth;
				q->max_legacy_llq_num = 0;
				q->max_legacy_llq_depth = 0;
				q->max_header_size = 512;
				q->max_packet_tx_descs = 8;
				q->max_packet_rx_descs = 8;
				hw->attrs_read = 1;
				filled = 1;
				break;
			}
			case ENA_ADMIN_LLQ: {
				struct ena_admin_feature_llq_desc *llq;

				llq = (struct ena_admin_feature_llq_desc *)
				      comp->response_specific_data;
				memset(llq, 0, sizeof(*llq));
				llq->max_llq_num = 16;
				llq->max_llq_depth = 1024;
				llq->header_location_ctrl_supported = 1;
				llq->entry_size_ctrl_supported = 1;
				hw->attrs_read = 1;
				filled = 1;
				break;
			}
			case 0:
				/* Legacy request without inline data */
				break;
			default:
				status = ENA_ADMIN_ILLEGAL_PARAMETER;
				break;
			}
		} else { /* ENA_ADMIN_SET_FEATURE */
			if (hw->require_attrs_first && !hw->attrs_read) {
				status = ENA_ADMIN_ILLEGAL_PARAMETER;
			} else if (feat->feature_id == ENA_ADMIN_MTU) {
				uint32_t mtu = feat->raw[0];

				if (mtu < ENA_INIT_MTU_MIN ||
				    mtu > hw->dev_max_mtu) {
					status = ENA_ADMIN_ILLEGAL_PARAMETER;
				} else {
					hw->negotiated_mtu = mtu;
				}
			} else if (feat->feature_id == ENA_ADMIN_HOST_ATTR_CONFIG) {
				uint64_t os_phys =
					(uint64_t)feat->raw[0] |
					((uint64_t)(feat->raw[1] & 0xFFFFu) << 32);

				if (os_phys != 0) {
					hw->host_info_base =
						(uint8_t *)(uintptr_t)os_phys;
					hw->host_info_debug_size = feat->raw[4];
				}
			} else if (feat->feature_id == ENA_ADMIN_LLQ) {
				filled = 1;
			} else {
				status = ENA_ADMIN_ILLEGAL_PARAMETER;
			}
		}
	}
	}

	comp->acq_common_desc.status = status;
	comp->acq_common_desc.extended_status = status;

	if (status == 0 && !filled) {
		/* Default success payload */
		for (i = 0; i < 14; i++)
			comp->response_specific_data[i] = 0x5E5E0000u | (uint32_t)i;
	}
}

/* Track driver ACQ consumption from the ACQ tail register (0x30) */
uint16_t mock_ena_hw_settle_acq_head(struct mock_ena_hw *hw)
{
	uint16_t mask;
	uint16_t reg;
	uint16_t depth;
	uint16_t delta;

	if (!hw)
		return 0;

	depth = (uint16_t)(mock_ena_hw_get_reg32(hw, ENA_REGS_ACQ_CAPS_OFF) & 0xFFFFu);
	if (depth == 0)
		depth = 8;

	mask = depth - 1;
	reg = (uint16_t)(mock_ena_hw_get_reg32(hw, ENA_REGS_ACQ_TAIL_OFF) & mask);

	if (reg != hw->last_acq_tail_reg) {
		delta = (uint16_t)((reg + depth - hw->last_acq_tail_reg) & mask);
		if (delta == 0)
			delta = depth;
		hw->drv_acq_head = (uint16_t)(hw->drv_acq_head + delta);
		hw->last_acq_tail_reg = reg;
	}

	return hw->drv_acq_head;
}

void mock_ena_hw_aq_doorbell_hook(void *cookie, uint16_t tail)
{
	struct mock_ena_hw *hw = (struct mock_ena_hw *)cookie;
	uint64_t aq_phys;
	uint64_t acq_phys;
	uint16_t aq_idx;
	uint16_t acq_idx;
	const struct ena_admin_aq_entry *req;
	struct ena_admin_acq_entry *comp;

	if (!hw)
		return;

	if (hw->admin_hang)
		return;

	mock_ena_hw_settle_acq_head(hw);

	/* Learn rings and depths from BAR0 */
	aq_phys = mock_read_phys(hw, ENA_REGS_AQ_BASE_LO_OFF,
				 ENA_REGS_AQ_BASE_HI_OFF);
	acq_phys = mock_read_phys(hw, ENA_REGS_ACQ_BASE_LO_OFF,
				  ENA_REGS_ACQ_BASE_HI_OFF);
	hw->dev_aq_base = (uint8_t *)(uintptr_t)aq_phys;
	hw->dev_acq_base = (uint8_t *)(uintptr_t)acq_phys;
	hw->dev_aq_depth = (uint16_t)mock_ena_hw_get_reg32(hw, ENA_REGS_AQ_CAPS_OFF);
	hw->dev_acq_depth = (uint16_t)mock_ena_hw_get_reg32(hw, ENA_REGS_ACQ_CAPS_OFF);

	if (!hw->dev_aq_base || !hw->dev_acq_base)
		return;
	if (hw->dev_aq_depth == 0 || hw->dev_acq_depth == 0)
		return;

	/* Consume requested AQ entry */
	aq_idx = (tail - 1) & (hw->dev_aq_depth - 1);
	req = (const struct ena_admin_aq_entry *)
	      (hw->dev_aq_base + (size_t)aq_idx * sizeof(*req));
	hw->last_opcode = req->aq_common_desc.opcode;
	hw->last_command_id = req->aq_common_desc.command_id & 0x0FFF;

	/* Write ACQ completion at device tail */
	acq_idx = hw->dev_acq_tail & (hw->dev_acq_depth - 1);
	comp = (struct ena_admin_acq_entry *)
	       (hw->dev_acq_base + (size_t)acq_idx * sizeof(*comp));
	memset(comp, 0, sizeof(*comp));
	comp->acq_common_desc.command =
		hw->inject_bad_cmd_id ? hw->bad_cmd_id : hw->last_command_id;
	comp->acq_common_desc.flags = hw->dev_acq_phase;
	comp->acq_common_desc.sq_head_indx = 0;

	mock_dispatch_feature(hw, req, comp);

	/* Advance device ACQ tail and flip phase on wrap */
	hw->dev_acq_tail++;
	if ((hw->dev_acq_tail & (hw->dev_acq_depth - 1)) == 0)
		hw->dev_acq_phase ^= 1;
}

void mock_ena_hw_set_admin_status(struct mock_ena_hw *hw, uint8_t status)
{
	if (hw)
		hw->admin_status = status;
}

void mock_ena_hw_hang_admin(struct mock_ena_hw *hw)
{
	if (hw)
		hw->admin_hang = 1;
}

void mock_ena_hw_clear_admin_hang(struct mock_ena_hw *hw)
{
	if (hw)
		hw->admin_hang = 0;
}

void mock_ena_hw_inject_bad_cmd_id(struct mock_ena_hw *hw, uint16_t id)
{
	if (hw) {
		hw->bad_cmd_id = (uint16_t)(id & 0x0FFF);
		hw->inject_bad_cmd_id = 1;
	}
}

void mock_ena_hw_clear_bad_cmd_id(struct mock_ena_hw *hw)
{
	if (hw)
		hw->inject_bad_cmd_id = 0;
}

void mock_ena_hw_inject_bad_db_offset(struct mock_ena_hw *hw, uint32_t offset)
{
	if (hw) {
		hw->bad_db_offset = offset;
		hw->inject_bad_db_offset = 1;
	}
}

void mock_ena_hw_clear_bad_db_offset(struct mock_ena_hw *hw)
{
	if (hw)
		hw->inject_bad_db_offset = 0;
}

void mock_ena_hw_inject_fake_req_id(struct mock_ena_hw *hw, uint16_t id)
{
	if (hw) {
		hw->fake_req_id = id;
		hw->inject_fake_req_id = 1;
	}
}

void mock_ena_hw_clear_fake_req_id(struct mock_ena_hw *hw)
{
	if (hw)
		hw->inject_fake_req_id = 0;
}

void mock_pci_inject_fault(struct mock_ena_hw *hw, enum mock_pci_fault_type type, uint64_t arg)
{
	if (!hw)
		return;

	switch (type) {
	case MOCK_PCI_FAULT_NONE:
		mock_pci_clear_faults(hw);
		break;
	case MOCK_PCI_FAULT_BAD_DB_OFFSET:
		hw->inject_bad_db_offset = 1;
		hw->bad_db_offset = (uint32_t)arg;
		break;
	case MOCK_PCI_FAULT_UNALIGNED_DB_OFFSET:
		hw->inject_bad_db_offset = 1;
		hw->bad_db_offset = (uint32_t)(arg | 1);
		break;
	case MOCK_PCI_FAULT_BAD_CMD_ID:
		hw->inject_bad_cmd_id = 1;
		hw->bad_cmd_id = (uint16_t)(arg & 0x0FFF);
		break;
	case MOCK_PCI_FAULT_FAKE_REQ_ID:
		hw->inject_fake_req_id = 1;
		hw->fake_req_id = (uint16_t)arg;
		break;
	case MOCK_PCI_FAULT_CORRUPT_LENGTH:
		hw->inject_corrupt_len = 1;
		hw->corrupt_len = (uint16_t)arg;
		break;
	case MOCK_PCI_FAULT_ADMIN_HANG:
		hw->admin_hang = 1;
		break;
	case MOCK_PCI_FAULT_ADMIN_STATUS:
		hw->admin_status = (uint8_t)arg;
		break;
	default:
		break;
	}
}

void mock_pci_clear_faults(struct mock_ena_hw *hw)
{
	if (!hw)
		return;

	hw->inject_bad_db_offset = 0;
	hw->bad_db_offset = 0;
	hw->inject_bad_cmd_id = 0;
	hw->bad_cmd_id = 0;
	hw->inject_fake_req_id = 0;
	hw->fake_req_id = 0;
	hw->inject_corrupt_len = 0;
	hw->corrupt_len = 0;
	hw->admin_hang = 0;
	hw->admin_status = 0;
}

void mock_ena_hw_require_attrs_first(struct mock_ena_hw *hw, int on)
{
	if (hw)
		hw->require_attrs_first = (uint8_t)(on ? 1 : 0);
}

void mock_ena_hw_inject_aenq(struct mock_ena_hw *hw, uint16_t group,
			     uint16_t syndrome)
{
	uint64_t aenq_phys;
	uint16_t aenq_idx;
	struct ena_admin_aenq_entry *ev;

	if (!hw)
		return;

	aenq_phys = mock_read_phys(hw, ENA_REGS_AENQ_BASE_LO_OFF,
				   ENA_REGS_AENQ_BASE_HI_OFF);
	hw->dev_aenq_base = (uint8_t *)(uintptr_t)aenq_phys;
	hw->dev_aenq_depth =
		(uint16_t)mock_ena_hw_get_reg32(hw, ENA_REGS_AENQ_CAPS_OFF);

	if (!hw->dev_aenq_base || hw->dev_aenq_depth == 0)
		return;

	aenq_idx = hw->dev_aenq_tail & (hw->dev_aenq_depth - 1);
	ev = (struct ena_admin_aenq_entry *)
	     (hw->dev_aenq_base + (size_t)aenq_idx * sizeof(*ev));
	memset(ev, 0, sizeof(*ev));
	ev->aenq_common_desc.group = group;
	ev->aenq_common_desc.syndrome = syndrome;
	ev->aenq_common_desc.flags = hw->dev_aenq_phase;
	ev->aenq_common_desc.timestamp_low = hw->dev_aenq_seq++;
	ev->aenq_common_desc.timestamp_high = 0;

	/* Advance device AENQ tail and flip phase on wrap */
	hw->dev_aenq_tail++;
	if ((hw->dev_aenq_tail & (hw->dev_aenq_depth - 1)) == 0)
		hw->dev_aenq_phase ^= 1;
}

void mock_ena_hw_emulate_tx(struct mock_ena_hw *hw, struct ena_ring *ring,
			    unsigned int count)
{
	struct ena_eth_io_tx_cdesc *cq_descs;
	unsigned int i;
	uint16_t sq_idx;
	uint16_t cq_idx;
	uint16_t req_id;
	uint16_t qid;

	if (!hw || !ring || !ring->cq_virt || count == 0)
		return;

	/* LLQ queues keep their descriptors in the BAR2 push buffer.
	 * Standard queues keep them in the host SQ ring. */
	if (!ring->is_llq && !ring->sq_virt)
		return;

	qid = ring->qid;
	if (qid >= MOCK_MAX_IO_QUEUES)
		qid = 0;

	cq_descs = (struct ena_eth_io_tx_cdesc *)ring->cq_virt;

	for (i = 0; i < count; i++) {
		const struct ena_eth_io_tx_desc *slot_desc;

		sq_idx = (ring->sq_head + (uint16_t)i) & (ring->sq_depth - 1);

		if (ring->is_llq && ring->push_buf_virt) {
			size_t entry_size = ring->llq_entry_size ?
				ring->llq_entry_size : 128;

			slot_desc = (const struct ena_eth_io_tx_desc *)
				((const uint8_t *)ring->push_buf_virt +
				 (size_t)sq_idx * entry_size);
		} else {
			slot_desc =
				&((struct ena_eth_io_tx_desc *)ring->sq_virt)[sq_idx];
		}

		req_id = (uint16_t)(((slot_desc->len_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK) >>
				     ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) << 10 |
				    ((slot_desc->meta_ctrl & ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK) >>
				     ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT));

		if (hw->inject_fake_req_id)
			req_id = hw->fake_req_id;

		cq_idx = hw->io_tx_cq_state[qid].cq_tail & (ring->cq_depth - 1);
		memset(&cq_descs[cq_idx], 0, sizeof(cq_descs[cq_idx]));
		cq_descs[cq_idx].req_id = req_id;
		cq_descs[cq_idx].status = 0;
		cq_descs[cq_idx].flags = hw->io_tx_cq_state[qid].cq_phase;
		cq_descs[cq_idx].sub_qid = ring->qid;
		cq_descs[cq_idx].sq_head_idx = (sq_idx + 1) & (ring->sq_depth - 1);

		hw->io_tx_cq_state[qid].cq_tail++;
		if ((hw->io_tx_cq_state[qid].cq_tail & (ring->cq_depth - 1)) == 0)
			hw->io_tx_cq_state[qid].cq_phase ^= 1;
	}
}

void mock_ena_hw_emulate_rx(struct mock_ena_hw *hw, struct ena_ring *ring,
			    unsigned int count, uint16_t pkt_len, uint32_t hash,
			    uint32_t status_flags)
{
	struct ena_eth_io_rx_desc *sq_descs;
	struct ena_eth_io_rx_cdesc_base *cq_descs;
	unsigned int i;
	uint16_t sq_idx;
	uint16_t cq_idx;
	uint16_t req_id;
	uint32_t status;
	uint16_t qid;

	if (!hw || !ring || !ring->sq_virt || !ring->cq_virt || count == 0)
		return;

	qid = ring->qid;
	if (qid >= MOCK_MAX_IO_QUEUES)
		qid = 0;

	sq_descs = (struct ena_eth_io_rx_desc *)ring->sq_virt;
	cq_descs = (struct ena_eth_io_rx_cdesc_base *)ring->cq_virt;

	for (i = 0; i < count; i++) {
		sq_idx = (ring->sq_head + (uint16_t)i) & (ring->sq_depth - 1);
		req_id = sq_descs[sq_idx].req_id;

		if (hw->inject_fake_req_id)
			req_id = hw->fake_req_id;

		cq_idx = hw->io_rx_cq_state[qid].cq_tail & (ring->cq_depth - 1);
		memset(&cq_descs[cq_idx], 0, sizeof(cq_descs[cq_idx]));

		status = ((uint32_t)hw->io_rx_cq_state[qid].cq_phase << ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT);
		status |= ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK |
			  ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK |
			  status_flags;

		cq_descs[cq_idx].status = status;
		cq_descs[cq_idx].length = hw->inject_corrupt_len ? hw->corrupt_len : pkt_len;
		cq_descs[cq_idx].req_id = req_id;
		cq_descs[cq_idx].hash = hash;
		cq_descs[cq_idx].sub_qid = ring->qid;

		hw->io_rx_cq_state[qid].cq_tail++;
		if ((hw->io_rx_cq_state[qid].cq_tail & (ring->cq_depth - 1)) == 0)
			hw->io_rx_cq_state[qid].cq_phase ^= 1;
	}

	ring->sq_head = (uint16_t)((ring->sq_head + (uint16_t)count) & (ring->sq_depth - 1));
}
