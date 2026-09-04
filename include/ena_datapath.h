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

/* Buffer sizes and memory limits */
/* Size of one RX buffer in bytes. RX is single-descriptor: each
 * receive buffer fills exactly one descriptor. A frame longer than
 * this size cannot be received. The driver drops such a completion
 * cleanly. Jumbo frame RX needs multi-descriptor RX reassembly
 * (roadmap Phase 12), which is not implemented. */
#define ENA_RX_BUF_SIZE         2048
#define ENA_TX_BOUNCE_SIZE      4096
/* Release a stuck TX bounce after this many transmit attempts without a
 * completion. Bounds the time a lost completion blocks low-memory transmit. */
#define ENA_TX_BOUNCE_STALL_LIMIT 256
#define ENA_DMA_LOW_MEM_LIMIT   0x100000ULL
#define ENA_NETDEV_IOALIGN      64
#define ENA_NETDEV_MAX_QUEUES   8
#define ENA_MIN_RING_DESC       4
#define ENA_DEFAULT_RING_DESC   256
#define ENA_MAX_RING_DESC       4096
#define ENA_DEFAULT_MTU         1500
#define ENA_MIN_MTU_LEN         68

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

/* Standard Completion Queue entry sizes in 32-bit words (reference driver). */
#define ENA_TX_CQ_ENTRY_SIZE_WORDS	2
#define ENA_RX_CQ_ENTRY_SIZE_WORDS	4

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

	/* True while the hardware queues behind this ring are valid. Cleared
	 * on device reset and on destroy; restored on create. Data path
	 * functions refuse to touch a ring whose hardware is invalid. */
	bool hw_valid;

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
	uint32_t cq_elem_size;     /* CQ entry stride in bytes (8 or 16) */
	uint16_t cq_idx;           /* Device-assigned CQ hardware ID */
	uint32_t cq_db_offset;     /* Offset from BAR0 to CQ doorbell / head DB */
	volatile uint32_t *cq_db;  /* Mapped CQ doorbell address */
	uint32_t cq_unmask_db_offset; /* BAR0 offset of the per-queue
	                                interrupt unmask register from the
	                                CREATE_CQ response */

	/* Request ID Free Pool (FIFO) */
	uint16_t *free_req_ids;    /* Array of available request IDs */
	uint16_t free_req_head;    /* Pop index */
	uint16_t free_req_tail;    /* Push index */
	uint16_t free_req_count;   /* Count of free IDs */
	uint8_t *req_in_flight;    /* In-flight flag for each request ID */
	uint32_t ring_lock;        /* Atomic spinlock for ring access */

	/* RX drop callback: return a dropped netbuf bounce slot to the pool
	 * and free the buffer. Set by the netdev layer, called from ena_rx_poll */
	void (*drop_netbuf_cb)(void *arg, void *netbuf);
	void *drop_netbuf_arg;

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
	uint32_t llq_entry_size;
};

static inline void ena_ring_lock(struct ena_ring *ring)
{
	while (__sync_lock_test_and_set(&ring->ring_lock, 1u) != 0u)
		ena_pause();
}

static inline void ena_ring_unlock(struct ena_ring *ring)
{
	__sync_lock_release(&ring->ring_lock);
}

/* -------------------------------------------------------------------------
 * Datapath Ring Lifecycle & Allocation Prototypes
 * ------------------------------------------------------------------------- */

/**
 * Allocate and initialize host memory structures for an IO ring pair.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param qid Hardware queue index to assign.
 * @param ring_type Direction of the ring (TX or RX).
 * @param sq_depth Submission queue depth in entries (must be a power of two).
 * @param cq_depth Completion queue depth in entries (must be a power of two).
 * @param out_ring Output pointer where the allocated ring pointer is stored.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_ring_alloc(struct ena_adapter *adapter, uint16_t qid,
		   enum ena_ring_type ring_type, uint16_t sq_depth,
		   uint16_t cq_depth, struct ena_ring **out_ring);

/**
 * Release host memory and buffer tracking structures for an IO ring.
 *
 * @param ring Pointer to the ring structure to free.
 */
void ena_ring_free(struct ena_ring *ring);

/**
 * Create hardware SQ and CQ instances on the device for this ring.
 *
 * @param ring Pointer to the initialized ring structure.
 * @param msix_vector MSI-X interrupt vector index to bind to the completion queue.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_ring_create_hw(struct ena_ring *ring, uint32_t msix_vector);

/**
 * Destroy hardware SQ and CQ instances on the device for this ring.
 *
 * @param ring Pointer to the active ring structure.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_ring_destroy_hw(struct ena_ring *ring);

/**
 * Allocate an available request ID from the ring free pool.
 *
 * @param ring Pointer to the ring structure.
 * @param out_req_id Output pointer where the allocated request ID is stored.
 * @return 0 on success, or -ENOSPC if no free request IDs remain.
 */
int ena_ring_req_id_alloc(struct ena_ring *ring, uint16_t *out_req_id);

/**
 * Return an in-flight request ID back to the ring free pool.
 *
 * @param ring Pointer to the ring structure.
 * @param req_id Request ID to release.
 * @return 0 on success, or -EINVAL if the request ID is invalid or not in-flight.
 */
int ena_ring_req_id_free(struct ena_ring *ring, uint16_t req_id);

/* Admin command helpers for CQ and SQ */

/**
 * Issue a CREATE_CQ admin command to register a Completion Queue with the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param cq_depth Completion queue depth in entries.
 * @param cq_phys Physical base address of the CQ descriptor ring.
 * @param msix_vector MSI-X vector index for completion interrupts.
 * @param entry_size_words Size of each CQ entry in 32-bit words.
 * @param out_cq_idx Output pointer for the device-assigned CQ hardware index.
 * @param out_db_offset Output pointer for the doorbell register byte offset.
 * @param out_unmask_off Output pointer for the per-queue interrupt unmask
 *        register byte offset, or NULL to ignore it.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_create_cq(struct ena_adapter *adapter, uint16_t cq_depth,
			uint64_t cq_phys, uint32_t msix_vector,
			uint8_t entry_size_words,
			uint16_t *out_cq_idx, uint32_t *out_db_offset,
			uint32_t *out_unmask_off);

/**
 * Issue a DESTROY_CQ admin command to remove a Completion Queue from the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param cq_idx Device hardware index of the CQ to destroy.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_destroy_cq(struct ena_adapter *adapter, uint16_t cq_idx);

/**
 * Issue a CREATE_SQ admin command to register a Submission Queue with the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param sq_depth Submission queue depth in entries.
 * @param sq_phys Physical base address of the SQ descriptor ring.
 * @param sq_head_wb_phys Physical address for SQ head pointer writeback.
 * @param cq_idx Hardware index of the paired Completion Queue.
 * @param direction Queue traffic direction (1 for TX, 2 for RX).
 * @param out_sq_idx Output pointer for the device-assigned SQ hardware index.
 * @param out_db_offset Output pointer for the doorbell register byte offset.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_create_sq(struct ena_adapter *adapter, uint16_t sq_depth,
			uint64_t sq_phys, uint64_t sq_head_wb_phys,
			uint16_t cq_idx, uint8_t direction,
			uint16_t *out_sq_idx, uint32_t *out_db_offset);

/**
 * Issue a DESTROY_SQ admin command to remove a Submission Queue from the device.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param sq_idx Device hardware index of the SQ to destroy.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_destroy_sq(struct ena_adapter *adapter, uint16_t sq_idx);

/**
 * Issue a CREATE_SQ admin command for a device-placement (LLQ) queue.
 * The queue lives in the device LLQ BAR2, so no host SQ address is passed.
 * The response provides the BAR2 offsets of the descriptor ring and the
 * header ring.
 *
 * @param adapter Pointer to the master ENA adapter structure.
 * @param sq_depth Submission queue depth in entries.
 * @param cq_idx Hardware index of the paired Completion Queue.
 * @param direction Queue traffic direction (1 for TX, 2 for RX).
 * @param out_sq_idx Output pointer for the device-assigned SQ hardware index.
 * @param out_db_offset Output pointer for the doorbell register byte offset.
 * @param out_llq_descs_off Output pointer for the LLQ BAR2 descriptor offset.
 * @param out_llq_headers_off Output pointer for the LLQ BAR2 header offset.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_admin_create_sq_llq(struct ena_adapter *adapter, uint16_t sq_depth,
			    uint16_t cq_idx, uint8_t direction,
			    uint16_t *out_sq_idx, uint32_t *out_db_offset,
			    uint32_t *out_llq_descs_off,
			    uint32_t *out_llq_headers_off);

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

/**
 * Calculate the number of unused descriptor slots in the TX Submission Queue.
 *
 * @param ring Pointer to the TX ring structure.
 * @return Number of available descriptor slots.
 */
uint16_t ena_tx_free_space(const struct ena_ring *ring);

/**
 * Write a transmit packet descriptor into the TX Submission Queue.
 *
 * @param ring Pointer to the TX ring structure.
 * @param pkt Pointer to the transmit packet metadata and buffer info.
 * @param out_req_id Output pointer for the assigned request ID.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_tx_submit(struct ena_ring *ring, const struct ena_tx_pkt *pkt,
		  uint16_t *out_req_id);

/**
 * Write the updated TX SQ tail index to the hardware MMIO doorbell register.
 *
 * @param ring Pointer to the TX ring structure.
 */
void ena_tx_doorbell(struct ena_ring *ring);

/**
 * Poll the TX Completion Queue for completed packets and release buffers.
 *
 * @param ring Pointer to the TX ring structure.
 * @param budget Maximum number of completions to process in this call.
 * @param cleaned_count Output pointer storing the count of processed completions.
 * @return 0 on success, or a negative errno value on error.
 */
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
	bool first;                /* First descriptor of a packet/LRO frame */
	bool last;                 /* Last descriptor of a packet/LRO frame */
	uint16_t req_id;           /* Request ID used for this buffer */
};

/* -------------------------------------------------------------------------
 * Receive (RX) Datapath Functions (Phase 6)
 * ------------------------------------------------------------------------- */

/**
 * Calculate the number of empty slots in the RX Submission Queue.
 *
 * @param ring Pointer to the RX ring structure.
 * @return Number of available descriptor slots.
 */
uint16_t ena_rx_free_space(const struct ena_ring *ring);

/**
 * Enqueue a single empty receive buffer descriptor into the RX Submission Queue.
 *
 * @param ring Pointer to the RX ring structure.
 * @param netbuf Pointer to the allocated network buffer object.
 * @param phys_addr Physical DMA address of the receive data buffer.
 * @param buf_len Usable capacity of the receive data buffer in bytes.
 * @param out_req_id Output pointer for the assigned request ID.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rx_submit_one(struct ena_ring *ring, void *netbuf, uint64_t phys_addr,
		      uint32_t buf_len, uint16_t *out_req_id);

/**
 * Batch-refill empty receive buffers into the RX Submission Queue.
 *
 * @param ring Pointer to the RX ring structure.
 * @param count Number of empty buffers to allocate and enqueue.
 * @param alloc_netbuf Callback function to allocate a network buffer.
 * @param alloc_arg User context pointer passed to the allocator callback.
 * @param refilled_count Output pointer storing the number of enqueued buffers.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_rx_refill(struct ena_ring *ring, unsigned int count,
		  void *(*alloc_netbuf)(void *arg, uint64_t *phys_out, uint32_t *len_out),
		  void *alloc_arg, unsigned int *refilled_count);

/**
 * Write the updated RX SQ tail index to the hardware MMIO doorbell register.
 *
 * @param ring Pointer to the RX ring structure.
 */
void ena_rx_doorbell(struct ena_ring *ring);

/**
 * Poll the RX Completion Queue for incoming packets and extract metadata.
 *
 * @param ring Pointer to the RX ring structure.
 * @param pkts Array of packet structures where received packet info is written.
 * @param max_pkts Maximum number of received packets to process.
 * @return Number of packets received, or a negative errno value on error.
 */
int ena_rx_poll(struct ena_ring *ring, struct ena_rx_pkt *pkts,
		unsigned int max_pkts);

#endif /* LIBENA_ENA_DATAPATH_H */

