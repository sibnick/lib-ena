/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_DATAPATH_H
#define LIBENA_ENA_DATAPATH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ena_plat.h"
#include "ena_admin.h"

/* Forward declaration */
struct ena_adapter;

/* Ring Types */
enum ena_ring_type {
	ENA_RING_TYPE_TX = 1,
	ENA_RING_TYPE_RX = 2
};

/* -------------------------------------------------------------------------
 * Hardware Descriptors (Matching reference/ena_eth_io_defs.h)
 * ------------------------------------------------------------------------- */

/* 16-byte Transmit Submission Descriptor */
struct ena_eth_io_tx_desc {
	uint32_t len_ctrl;
	uint32_t meta_ctrl;
	uint32_t buff_addr_lo;
	uint32_t buff_addr_hi_hdr_sz;
};

/* 8-byte Transmit Completion Descriptor */
struct ena_eth_io_tx_cdesc {
	uint16_t req_id;
	uint8_t status;
	uint8_t flags;         /* bit 0: phase */
	uint16_t sub_qid;
	uint16_t sq_head_idx;
};

/* 16-byte Receive Submission Descriptor */
struct ena_eth_io_rx_desc {
	uint16_t length;
	uint8_t reserved2;
	uint8_t ctrl;          /* bit 0: phase, bit 2: first, bit 3: last, bit 4: comp_req */
	uint16_t req_id;
	uint16_t reserved6;
	uint32_t buff_addr_lo;
	uint16_t buff_addr_hi;
	uint16_t reserved16_w3;
};

/* 16-byte Receive Completion Descriptor (4 words) */
struct ena_eth_io_rx_cdesc_base {
	uint32_t status;       /* bit 24: phase, bit 26: first, bit 27: last */
	uint16_t length;
	uint16_t req_id;
	uint32_t hash;
	uint16_t sub_qid;
	uint8_t offset;
	uint8_t reserved;
};

/* 32-byte Extended Receive Completion Descriptor (8 words) */
struct ena_eth_io_rx_cdesc_ext {
	struct ena_eth_io_rx_cdesc_base base;
	uint32_t buff_addr_lo;
	uint16_t buff_addr_hi;
	uint16_t reserved16;
	uint32_t reserved_w6;
	uint32_t reserved_w7;
};

/* -------------------------------------------------------------------------
 * Hardware Descriptor Bitmasks and Shifts
 * ------------------------------------------------------------------------- */

/* TX Submission Descriptor len_ctrl masks */
#define ENA_ETH_IO_TX_DESC_LENGTH_MASK           0x0000FFFFu
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT       16
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK        0x003F0000u
#define ENA_ETH_IO_TX_DESC_PHASE_SHIFT           24
#define ENA_ETH_IO_TX_DESC_PHASE_MASK            0x01000000u
#define ENA_ETH_IO_TX_DESC_FIRST_SHIFT           26
#define ENA_ETH_IO_TX_DESC_FIRST_MASK            0x04000000u
#define ENA_ETH_IO_TX_DESC_LAST_SHIFT            27
#define ENA_ETH_IO_TX_DESC_LAST_MASK             0x08000000u
#define ENA_ETH_IO_TX_DESC_COMP_REQ_SHIFT        28
#define ENA_ETH_IO_TX_DESC_COMP_REQ_MASK         0x10000000u

/* TX Submission Descriptor meta_ctrl masks */
#define ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK     0x0000000Fu
#define ENA_ETH_IO_TX_DESC_DF_MASK               0x00000010u
#define ENA_ETH_IO_TX_DESC_TSO_EN_MASK           0x00000080u
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT    8
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK     0x00001F00u
#define ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK       0x00002000u
#define ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK       0x00004000u
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT       22
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK        0xFFC00000u

/* TX Completion Descriptor flags */
#define ENA_ETH_IO_TX_CDESC_PHASE_MASK           0x01u

/* RX Submission Descriptor ctrl masks */
#define ENA_ETH_IO_RX_DESC_PHASE_MASK            0x01u
#define ENA_ETH_IO_RX_DESC_FIRST_MASK            0x04u
#define ENA_ETH_IO_RX_DESC_LAST_MASK             0x08u
#define ENA_ETH_IO_RX_DESC_COMP_REQ_MASK         0x10u

/* RX Completion Descriptor status masks */
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK    0x00002000u
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK    0x00004000u
#define ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK      0x00008000u
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK 0x00010000u
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT         24
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK          0x01000000u
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_SHIFT         26
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK          0x04000000u
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_SHIFT          27
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK           0x08000000u

/* -------------------------------------------------------------------------
 * Software Buffer Tracking Structures
 * ------------------------------------------------------------------------- */

/* Per-packet tracking for Transmit buffers */
struct ena_tx_buffer {
	void *netbuf;              /* Pointer to struct uk_netbuf */
	uint64_t phys_addr;        /* Physical DMA address of payload */
	uint32_t data_len;         /* Length of packet buffer */
	uint16_t num_descs;        /* Number of descriptors used */
	uint16_t req_id;           /* Request ID */
};

/* Per-packet tracking for Receive buffers */
struct ena_rx_buffer {
	void *netbuf;              /* Pointer to struct uk_netbuf */
	uint64_t phys_addr;        /* Physical DMA address of receive buffer */
	uint32_t data_len;         /* Buffer capacity */
	uint16_t req_id;           /* Request ID */
};

/* Circular Ring Abstraction (manages SQ, CQ, and buffer tracking) */
struct ena_ring {
	struct ena_adapter *adapter;
	uint16_t qid;              /* Queue index */
	enum ena_ring_type ring_type;

	/* Submission Queue (SQ) */
	void *sq_virt;             /* Virtual address of SQ DMA ring */
	uint64_t sq_phys;          /* Physical address of SQ DMA ring */
	void *sq_head_wb_virt;     /* Virtual address of SQ head writeback */
	uint64_t sq_head_wb_phys;  /* Physical address of SQ head writeback */
	uint16_t sq_depth;         /* SQ ring depth (must be power of 2) */
	uint16_t sq_tail;          /* Producer tail index */
	uint16_t sq_head;          /* Consumer head index */
	uint8_t sq_phase;          /* Current SQ phase bit (1 or 0) */
	uint16_t sq_idx;           /* Device-assigned SQ hardware ID */
	uint32_t sq_db_offset;     /* Offset from BAR0 to SQ doorbell */
	volatile uint32_t *sq_db;  /* Mapped SQ doorbell address */

	/* Completion Queue (CQ) */
	void *cq_virt;             /* Virtual address of CQ DMA ring */
	uint64_t cq_phys;          /* Physical address of CQ DMA ring */
	uint16_t cq_depth;         /* CQ ring depth (must be power of 2) */
	uint16_t cq_head;          /* Consumer head index */
	uint8_t cq_phase;          /* Expected CQ phase bit (starts at 1) */
	uint16_t cq_idx;           /* Device-assigned CQ hardware ID */
	uint32_t cq_db_offset;     /* Offset from BAR0 to CQ doorbell / head DB */
	volatile uint32_t *cq_db;  /* Mapped CQ doorbell address */

	/* Request ID Free Pool (FIFO) */
	uint16_t *free_req_ids;    /* Array of available request IDs */
	uint16_t free_req_head;    /* Pop index */
	uint16_t free_req_tail;    /* Push index */
	uint16_t free_req_count;   /* Count of free IDs */
	uint8_t *req_in_flight;    /* In-flight flag for each request ID */
	uint32_t ring_lock;        /* Atomic spinlock for ring access */

	/* Tracking Buffers (allocated to depth entries, indexed by req_id) */
	union {
		struct ena_tx_buffer *tx_bufs;
		struct ena_rx_buffer *rx_bufs;
		void *raw_bufs;
	} buffers;

	/* Datapath Statistics */
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	/* Phase 9: Low Latency Queue (LLQ) metadata */
	bool is_llq;
	void *push_buf_virt;
	uint64_t push_buf_phys;
	uint32_t push_buf_size;
	uint32_t llq_header_len;
};

static inline void ena_ring_lock(struct ena_ring *ring)
{
	while (__sync_lock_test_and_set(&ring->ring_lock, 1u) != 0u)
		ena_delay_us(1);
}

static inline void ena_ring_unlock(struct ena_ring *ring)
{
	__sync_lock_release(&ring->ring_lock);
}

/* -------------------------------------------------------------------------
 * Datapath Ring Lifecycle & Allocation Prototypes
 * ------------------------------------------------------------------------- */

int ena_ring_alloc(struct ena_adapter *adapter, uint16_t qid,
		   enum ena_ring_type ring_type, uint16_t sq_depth,
		   uint16_t cq_depth, struct ena_ring **out_ring);

void ena_ring_free(struct ena_ring *ring);

int ena_ring_create_hw(struct ena_ring *ring, uint32_t msix_vector);

int ena_ring_destroy_hw(struct ena_ring *ring);

int ena_ring_req_id_alloc(struct ena_ring *ring, uint16_t *out_req_id);

int ena_ring_req_id_free(struct ena_ring *ring, uint16_t req_id);

/* Admin command helpers for CQ and SQ */
int ena_admin_create_cq(struct ena_adapter *adapter, uint16_t cq_depth,
			uint64_t cq_phys, uint32_t msix_vector,
			uint8_t entry_size_words,
			uint16_t *out_cq_idx, uint32_t *out_db_offset);

int ena_admin_destroy_cq(struct ena_adapter *adapter, uint16_t cq_idx);

int ena_admin_create_sq(struct ena_adapter *adapter, uint16_t sq_depth,
			uint64_t sq_phys, uint64_t sq_head_wb_phys,
			uint16_t cq_idx, uint8_t direction,
			uint16_t *out_sq_idx, uint32_t *out_db_offset);

int ena_admin_destroy_sq(struct ena_adapter *adapter, uint16_t sq_idx);

/* Protocol Indexes (matching reference/ena_eth_io_defs.h) */
enum ena_eth_io_l3_proto_index {
	ENA_ETH_IO_L3_PROTO_UNKNOWN = 0,
	ENA_ETH_IO_L3_PROTO_IPV4    = 8,
	ENA_ETH_IO_L3_PROTO_IPV6    = 11,
	ENA_ETH_IO_L3_PROTO_FCOE    = 21,
	ENA_ETH_IO_L3_PROTO_ROCE    = 22,
};

enum ena_eth_io_l4_proto_index {
	ENA_ETH_IO_L4_PROTO_UNKNOWN        = 0,
	ENA_ETH_IO_L4_PROTO_TCP            = 12,
	ENA_ETH_IO_L4_PROTO_UDP            = 13,
	ENA_ETH_IO_L4_PROTO_ROUTEABLE_ROCE = 23,
};

/* Transmit packet submission descriptor structure */
struct ena_tx_pkt {
	void *netbuf;              /* Pointer to struct uk_netbuf */
	uint64_t phys_addr;        /* Physical DMA address of payload */
	uint32_t len;              /* Length of packet payload in bytes */
	uint8_t l3_proto;          /* enum ena_eth_io_l3_proto_index */
	uint8_t l4_proto;          /* enum ena_eth_io_l4_proto_index */
	bool l3_csum_en;           /* Enable IPv4 checksum offload */
	bool l4_csum_en;           /* Enable TCP/UDP checksum offload */
	bool df;                   /* Don't fragment flag for IPv4 */
	bool tso_en;               /* TCP segmentation offload flag */
};

/* -------------------------------------------------------------------------
 * Transmit (TX) Datapath Functions (Phase 5)
 * ------------------------------------------------------------------------- */

/* Return number of available submission slots in the TX ring */
uint16_t ena_tx_free_space(const struct ena_ring *ring);

/* Serialize a transmit packet into the TX Submission Queue */
int ena_tx_submit(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		  uint16_t *out_req_id);

/* Ring the hardware TX MMIO doorbell */
void ena_tx_doorbell(struct ena_ring *ring);

/* Poll TX Completion Queue for finished packets and recycle buffers */
int ena_tx_poll_completions(struct ena_ring *ring, unsigned int budget,
			    unsigned int *cleaned_count);

/* Received packet structure */
struct ena_rx_pkt {
	void *netbuf;              /* Pointer to struct uk_netbuf */
	uint32_t len;              /* Received packet length in bytes */
	uint32_t hash;             /* RSS packet hash */
	bool l3_csum_err;          /* L3 checksum error detected */
	bool l4_csum_err;          /* L4 checksum error detected */
	bool l4_csum_checked;      /* L4 checksum was checked by hardware */
	bool frag;                 /* IPv4 fragmented packet */
	uint16_t req_id;           /* Request ID used for this buffer */
};

/* -------------------------------------------------------------------------
 * Receive (RX) Datapath Functions (Phase 6)
 * ------------------------------------------------------------------------- */

/* Return number of free slots available in the RX Submission Queue */
uint16_t ena_rx_free_space(const struct ena_ring *ring);

/* Enqueue a single empty receive buffer into the RX Submission Queue */
int ena_rx_submit_one(struct ena_ring *ring, void *netbuf, uint64_t phys_addr,
		      uint32_t buf_len, uint16_t *out_req_id);

/* Batch-replenish empty receive buffers into the RX Submission Queue */
int ena_rx_refill(struct ena_ring *ring, unsigned int count,
		  void *(*alloc_netbuf)(void *arg, uint64_t *phys_out, uint32_t *len_out),
		  void *alloc_arg, unsigned int *refilled_count);

/* Ring the hardware RX MMIO doorbell */
void ena_rx_doorbell(struct ena_ring *ring);

/* Poll the RX Completion Queue for incoming packets */
int ena_rx_poll(struct ena_ring *ring, struct ena_rx_pkt *pkts,
		unsigned int max_pkts);

#endif /* LIBENA_ENA_DATAPATH_H */
