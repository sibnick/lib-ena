/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_H
#define LIBENA_ENA_H

#include "ena_plat.h"
#include "ena_regs.h"
#include "ena_admin.h"
#include "ena_init.h"
#include "ena_datapath.h"
#include "ena_llq.h"

/* Forward declarations */
struct ena_adapter;
struct ena_ring;
struct ena_irq_vector;

/* Device operational states */
enum ena_state {
	ENA_STATE_UNINITIALIZED = 0,
	ENA_STATE_PCI_PROBED,
	ENA_STATE_ADMIN_READY,
	ENA_STATE_CONFIGURED,
	ENA_STATE_RUNNING,
	ENA_STATE_STOPPED,
	ENA_STATE_ERROR
};

/* PCI device ID table entry */
struct ena_pci_id {
	uint16_t vendor_id;
	uint16_t device_id;
};

/* ENA Master Adapter Context */
struct ena_adapter {
	void *pci_dev;                  /* Pointer to struct uk_pci_device */
	volatile uint8_t *bar0_base;    /* Virtual address of mapped BAR0 */
	size_t bar0_size;               /* Size of BAR0 MMIO space */
	volatile uint8_t *bar2_base;    /* Virtual address of LLQ BAR2 (optional) */
	size_t bar2_size;               /* Size of BAR2 MMIO space */
	
	enum ena_state state;           /* Driver lifecycle state */
	uint32_t version;               /* Device version from ENA_REGS_VERSION_OFF */
	uint32_t controller_version;    /* Controller version */
	uint32_t caps;                  /* Hardware capabilities */
	
	uint8_t mac_addr[6];            /* MAC address */
	uint16_t max_mtu;               /* Maximum supported MTU */
	uint16_t max_tx_queues;         /* Hardware maximum TX queues */
	uint16_t max_rx_queues;         /* Hardware maximum RX queues */
	uint16_t max_tx_ring_size;      /* Hardware maximum TX ring depth */
	uint16_t max_rx_ring_size;      /* Hardware maximum RX ring depth */

	/* Phase 2: Admin Queue (request ring) */
	void *aq_base;                  /* Virtual base of the AQ ring */
	uint64_t aq_phys;               /* Physical base of the AQ ring */
	uint16_t aq_depth;              /* AQ depth (power of 2) */
	uint16_t aq_tail;               /* Next AQ slot to fill */
	uint8_t aq_phase;               /* AQ phase bit for submitted entries */
	uint16_t next_command_id;       /* Next command id to assign */
	uint32_t admin_lock;            /* Busy flag serializing exec_cmd */

	/* Phase 2: Admin Completion Queue (response ring) */
	void *acq_base;                 /* Virtual base of the ACQ ring */
	uint64_t acq_phys;              /* Physical base of the ACQ ring */
	uint16_t acq_depth;             /* ACQ depth (power of 2) */
	uint16_t acq_head;             /* Next ACQ slot to read */
	uint8_t acq_phase;             /* Expected ACQ phase bit */

	/* Phase 2: Asynchronous Event Notification Queue */
	void *aenq_base;               /* Virtual base of the AENQ ring */
	uint64_t aenq_phys;            /* Physical base of the AENQ ring */
	uint16_t aenq_depth;           /* AENQ depth (power of 2) */
	uint16_t aenq_head;            /* Next AENQ slot to read */
	uint8_t aenq_phase;            /* Expected AENQ phase bit */

	/* Phase 2: AENQ event dispatch */
	ena_aenq_handler *aenq_handler;
	void *aenq_handler_arg;

	/* Phase 3: negotiated device attributes */
	uint32_t impl_id;               /* Device implementation id */
	uint32_t device_version;        /* Device version from attributes */
	uint32_t supported_features;    /* Feature id bitmap from device */
	uint32_t attr_caps;             /* Capability bitmap from attributes */
	uint32_t phys_addr_width;       /* Physical address width in bits */
	uint32_t virt_addr_width;       /* Virtual address width in bits */
	uint32_t max_header_size;       /* Maximum TX header size */
	uint16_t max_packet_tx_descs;   /* Max descriptors per TX packet */
	uint16_t max_packet_rx_descs;   /* Max descriptors per RX packet */
	uint32_t mtu;                   /* Negotiated MTU */

	/* Phase 3: host info buffer */
	void *host_info_base;           /* Virtual base of the 4KB buffer */
	uint64_t host_info_phys;        /* Physical base of the 4KB buffer */

	/* Phase 4: IO Rings */
	struct ena_ring **tx_rings;
	struct ena_ring **rx_rings;
	uint16_t num_tx_rings;
	uint16_t num_rx_rings;

	/* Phase 8: Interrupts and MSI-X vectors */
	struct ena_irq_vector *irq_vectors;
	uint32_t num_irq_vectors;

	/* Phase 9: Low Latency Queue (LLQ) */
	struct ena_llq_info llq_info;
};

/* Function prototypes for Phase 1 */

/**
 * Check if a PCI vendor and device ID pair matches a supported ENA device.
 *
 * @param vendor_id PCI vendor identifier.
 * @param device_id PCI device identifier.
 * @return 1 if the ID pair matches an ENA device, 0 otherwise.
 */
int ena_pci_match_id(uint16_t vendor_id, uint16_t device_id);

/**
 * Trigger a hardware controller reset via the DEV_CTL register.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_device_reset(struct ena_adapter *adapter);

/**
 * Poll the DEV_STS register until the device finishes reset.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param max_polls Maximum number of polling iterations before timing out.
 * @return 0 when reset completes, or -ETIMEDOUT if polling budget expires.
 */
int ena_device_wait_reset_complete(struct ena_adapter *adapter, unsigned int max_polls);

/**
 * Check if the device is in a ready state.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @return 0 if the device is ready, or -ENODEV if not ready.
 */
int ena_device_check_ready(const struct ena_adapter *adapter);

/**
 * Initialize base adapter structure and map BAR0 MMIO space.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param bar0_base Virtual memory address where BAR0 MMIO registers are mapped.
 * @param bar0_size Size of the BAR0 MMIO address space in bytes.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_device_init_scaffold(struct ena_adapter *adapter, void *bar0_base, size_t bar0_size);

#ifndef __Unikraft__
/* Host test hook: the mock registers a callback that observes each reset
 * status poll, so it can model a reset that finishes after N polls. */
typedef void ena_reset_poll_hook(void *cookie);
void ena_device_set_reset_poll_hook(ena_reset_poll_hook *hook, void *cookie);
#endif

/* Function prototypes for Phase 2 (Admin Queue and AENQ) */

/**
 * Initialize Admin Queue (AQ), Admin Completion Queue (ACQ), and AENQ.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param aq_depth Submission queue depth in entries (must be a power of two).
 * @param acq_depth Completion queue depth in entries (must be a power of two).
 * @param aenq_depth Event queue depth in entries (must be a power of two).
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_init(struct ena_adapter *adapter, uint16_t aq_depth,
		   uint16_t acq_depth, uint16_t aenq_depth);

/**
 * Release all Admin Queue, Completion Queue, and AENQ resources.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 */
void ena_admin_fini(struct ena_adapter *adapter);

/**
 * Submit an admin command and poll synchronously for completion.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param opcode Admin command operation code.
 * @param req Pointer to the command request payload buffer.
 * @param req_len Size of the request payload buffer in bytes.
 * @param resp Pointer to the buffer where response data is written.
 * @param resp_cap Capacity of the response buffer in bytes.
 * @param out_command_id Optional pointer to store the assigned command ID.
 * @param max_polls Maximum polling loops to wait for command completion.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_exec_cmd(struct ena_adapter *adapter, uint8_t opcode,
		       const void *req, size_t req_len, void *resp,
		       size_t resp_cap, uint16_t *out_command_id,
		       unsigned int max_polls);

/**
 * Register a callback handler for Asynchronous Event Notifications (AENQ).
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param handler Function pointer to the event notification callback.
 * @param arg User context pointer passed to the callback handler.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_aenq_register(struct ena_adapter *adapter,
			    ena_aenq_handler *handler, void *arg);

/**
 * Poll and dispatch pending asynchronous events from the AENQ ring.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param max_events Maximum number of event entries to process in this sweep.
 * @return Number of processed events on success, or a negative errno value on error.
 */
int ena_admin_aenq_poll(struct ena_adapter *adapter, unsigned int max_events);

#endif /* LIBENA_ENA_H */

