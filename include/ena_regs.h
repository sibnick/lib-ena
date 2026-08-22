/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_REGS_H
#define LIBENA_ENA_REGS_H

/* Amazon PCI Vendor ID */
#define ENA_PCI_VENDOR_ID                   0x1D0Fu

/* Supported ENA PCI Device IDs */
#define ENA_PCI_DEV_ID_RESERVED             0x0051u
#define ENA_PCI_DEV_ID_PF                   0x0EC2u
#define ENA_PCI_DEV_ID_LLQ_PF               0x1EC2u
#define ENA_PCI_DEV_ID_VF                   0xEC20u
#define ENA_PCI_DEV_ID_LLQ_VF               0xEC21u

/* ENA MMIO BAR0 Register Offsets */
#define ENA_REGS_VERSION_OFF                0x00u
#define ENA_REGS_CONTROLLER_VERSION_OFF     0x04u
#define ENA_REGS_CAPS_OFF                   0x08u
#define ENA_REGS_CAPS_EXT_OFF               0x0Cu
#define ENA_REGS_AQ_BASE_LO_OFF             0x10u
#define ENA_REGS_AQ_BASE_HI_OFF             0x14u
#define ENA_REGS_AQ_CAPS_OFF                0x18u
#define ENA_REGS_ACQ_BASE_LO_OFF            0x20u
#define ENA_REGS_ACQ_BASE_HI_OFF            0x24u
#define ENA_REGS_ACQ_CAPS_OFF               0x28u
#define ENA_REGS_AQ_DB_OFF                  0x2Cu
#define ENA_REGS_ACQ_TAIL_OFF               0x30u
#define ENA_REGS_AENQ_CAPS_OFF              0x34u
#define ENA_REGS_AENQ_BASE_LO_OFF           0x38u
#define ENA_REGS_AENQ_BASE_HI_OFF           0x3Cu
#define ENA_REGS_AENQ_HEAD_DB_OFF           0x40u
#define ENA_REGS_AENQ_TAIL_OFF              0x44u
#define ENA_REGS_INTR_MASK_OFF              0x4Cu
#define ENA_REGS_DEV_CTL_OFF                0x54u
#define ENA_REGS_DEV_STS_OFF                0x58u
#define ENA_REGS_MMIO_REG_READ_OFF          0x5Cu
#define ENA_REGS_MMIO_RESP_LO_OFF           0x60u
#define ENA_REGS_MMIO_RESP_HI_OFF           0x64u
#define ENA_REGS_RSS_IND_ENTRY_UPDATE_OFF   0x68u
#define ENA_REGS_PHC_DB_OFF                 0x100u

/* AQ CAPS register: bits 15:0 depth, bits 31:16 entry size in bytes. */
#define ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK            0x0000FFFFu
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT      16
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_MASK       0xFFFF0000u

/* ACQ CAPS register: bits 15:0 depth, bits 31:16 entry size in bytes. */
#define ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK          0x0000FFFFu
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT    16
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_MASK     0xFFFF0000u

/* AENQ CAPS register: bits 15:0 depth, bits 31:16 entry size in bytes. */
#define ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK        0x0000FFFFu
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT  16
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_MASK   0xFFFF0000u

/* Device Control Register Masks */
#define ENA_DEV_CTL_DEV_RESET_MASK          0x00000001u
#define ENA_DEV_CTL_AQ_RESTART_MASK         0x00000002u
#define ENA_DEV_CTL_QUIESCENT_MASK          0x00000004u
#define ENA_DEV_CTL_RESET_REASON_SHIFT      28
#define ENA_DEV_CTL_RESET_REASON_MASK       0xF0000000u

/* Device Status Register Masks */
#define ENA_DEV_STS_READY_MASK              0x00000001u
#define ENA_DEV_STS_AQ_RESTART_IN_PROG_MASK 0x00000002u
#define ENA_DEV_STS_AQ_RESTART_FIN_MASK     0x00000004u
#define ENA_DEV_STS_RESET_IN_PROG_MASK      0x00000008u
#define ENA_DEV_STS_RESET_FIN_MASK          0x00000010u
#define ENA_DEV_STS_FATAL_ERROR_MASK        0x00000020u

#endif /* LIBENA_ENA_REGS_H */
