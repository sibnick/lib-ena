/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_ENA_NETDEV_H
#define LIBENA_ENA_NETDEV_H

#include "ena.h"
#include "ena_datapath.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __Unikraft__
#include <uk/alloc.h>
#include <uk/bus/pci.h>
#include <uk/netdev.h>
#include <uk/netdev_core.h>
#include <uk/netdev_driver.h>
#include <uk/netbuf.h>
#else
struct uk_netbuf;
typedef uint16_t (*uk_netdev_alloc_rxpkts)(void *argp, struct uk_netbuf *pkts[], uint16_t count);
#endif

struct uk_netdev_rx_queue {
	struct ena_ring *ring;
	uint16_t queue_id;
	struct ena_adapter *adapter;
	void *allocator;
	uk_netdev_alloc_rxpkts alloc_rxpkts;
	void *alloc_rxpkts_argp;
	void *bounce_buf;
	uint64_t bounce_phys;
	uint16_t nb_desc;
	uint16_t bounce_free_head;
	uint16_t bounce_free_tail;
	uint16_t bounce_free_count;
	uint16_t *bounce_free_ids;
};

struct uk_netdev_tx_queue {
	struct ena_ring *ring;
	uint16_t queue_id;
	struct ena_adapter *adapter;
	void *allocator;
	void *bounce_buf;
	uint64_t bounce_phys;
	bool bounce_in_use;
	uint16_t bounce_req_id;
	uint16_t nb_desc;
};

#ifdef __Unikraft__

struct ena_uk_device {
	struct uk_netdev netdev;
	struct ena_adapter adapter;
	struct pci_device *pdev;
	void *bar0_vaddr;
	void *bar2_vaddr;
	struct uk_netdev_rx_queue rx_queues[ENA_NETDEV_MAX_QUEUES];
	struct uk_netdev_tx_queue tx_queues[ENA_NETDEV_MAX_QUEUES];
	uint16_t uid;
};

#define to_enadevice(ndev) \
	__containerof(ndev, struct ena_uk_device, netdev)

extern const struct uk_netdev_ops ena_ops;

/**
 * Receive a single packet from the specified RX queue (Unikraft native mode).
 *
 * @param dev Pointer to the network device.
 * @param queue Pointer to the RX queue structure.
 * @param pkt Pointer where the received network buffer is stored.
 * @return Positive status flag on packet receipt, 0 if queue is empty, or negative errno on error.
 */
int ena_netdev_rx_one(struct uk_netdev *dev, struct uk_netdev_rx_queue *queue, struct uk_netbuf **pkt);

/**
 * Transmit a single packet on the specified TX queue (Unikraft native mode).
 *
 * @param dev Pointer to the network device.
 * @param queue Pointer to the TX queue structure.
 * @param pkt Pointer to the network buffer to transmit.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_netdev_tx_one(struct uk_netdev *dev, struct uk_netdev_tx_queue *queue, struct uk_netbuf *pkt);

/**
 * Get the current link state from the AENQ LINK_CHANGE events.
 *
 * @param dev Pointer to the network device.
 * @return true when the link is up, false when it is down.
 */
bool ena_netdev_link_get(struct uk_netdev *dev);

/**
 * Tear down all driver-owned resources of the device: hardware queues
 * (SQ and CQ of every ring, LLQ included), bounce buffers, software
 * rings, MSI-X vectors, and admin queues.
 *
 * @param edev Pointer to the ENA device structure.
 */
void ena_netdev_teardown(struct ena_uk_device *edev);

/**
 * Release all driver-owned resources of a probed ENA device and detach
 * it from the driver registry. The pinned UK netdev core has no
 * unregister API, so the ENA device structure itself is not freed.
 *
 * @param pdev Pointer to the PCI device being removed.
 */
void ena_pci_remove_dev(struct pci_device *pdev);

#else /* !__Unikraft__ */

#define UK_NETDEV_MAC_ADDR_LEN 6

/* Network device operating states */
enum uk_netdev_state {
	UK_NETDEV_UNCONFIGURED = 0,
	UK_NETDEV_CONFIGURED   = 1,
	UK_NETDEV_RUNNING      = 2,
	UK_NETDEV_STOPPED      = 3,
};

/* Feature flags */
#define UK_NETDEV_F_RX_CSUM (1u << 0)
#define UK_NETDEV_F_TX_CSUM (1u << 1)

/* Hardware and driver capabilities */
struct uk_netdev_info {
	uint16_t max_rx_queues;
	uint16_t max_tx_queues;
	uint16_t max_mtu;
	uint16_t min_mtu;
	uint16_t mtu;
	uint8_t  hwaddr[UK_NETDEV_MAC_ADDR_LEN];
	uint32_t features;
};

/* Device configuration */
struct uk_netdev_conf {
	uint16_t nb_rx_queues;
	uint16_t nb_tx_queues;
};

/* Queue configuration */
struct uk_netdev_rxqueue_conf {
	uint16_t nb_desc;
	void *allocator;
	uk_netdev_alloc_rxpkts alloc_rxpkts;
	void *alloc_rxpkts_argp;
};

struct uk_netdev_txqueue_conf {
	uint16_t nb_desc;
	void *allocator;
};

/* Network buffer abstraction */
struct uk_netbuf {
	void *data;
	size_t len;
	size_t buflen;
	uint64_t phys_addr;
	uint32_t hash;
	bool l3_csum_err;
	bool l4_csum_err;
	bool l4_csum_checked;
	void *priv;
};

struct uk_netdev;

/* Unikraft netdev driver operations */
struct uk_netdev_ops {
	int (*info_get)(struct uk_netdev *dev, struct uk_netdev_info *info);
	int (*configure)(struct uk_netdev *dev, const struct uk_netdev_conf *conf);
	int (*rxq_configure)(struct uk_netdev *dev, uint16_t queue_id,
			     uint16_t nb_desc, const struct uk_netdev_rxqueue_conf *conf);
	int (*txq_configure)(struct uk_netdev *dev, uint16_t queue_id,
			     uint16_t nb_desc, const struct uk_netdev_txqueue_conf *conf);
	int (*dev_start)(struct uk_netdev *dev);
	int (*dev_stop)(struct uk_netdev *dev);
	int (*rxq_recv)(struct uk_netdev *dev, uint16_t queue_id, struct uk_netbuf **pkt);
	int (*txq_xmit)(struct uk_netdev *dev, uint16_t queue_id, struct uk_netbuf *pkt);
};

/* Main Unikraft network device object */
struct uk_netdev {
	const struct uk_netdev_ops *ops;
	enum uk_netdev_state state;
	struct uk_netdev_info info;
	uint16_t nb_rx_queues;
	uint16_t nb_tx_queues;
	struct ena_adapter *adapter;
	void *rx_allocator_arg;
	struct uk_netdev_rx_queue rx_queues[ENA_NETDEV_MAX_QUEUES];
	struct uk_netdev_tx_queue tx_queues[ENA_NETDEV_MAX_QUEUES];
};

/**
 * Allocate and initialize a network device structure for an ENA adapter.
 *
 * @param adapter Pointer to the master ENA adapter.
 * @return Pointer to the allocated network device, or NULL on allocation failure.
 */
struct uk_netdev *ena_netdev_alloc(struct ena_adapter *adapter);

/**
 * Free a network device structure and release associated ring resources.
 *
 * @param netdev Pointer to the network device structure to free.
 */
void ena_netdev_free(struct uk_netdev *netdev);

/**
 * Register a network device with the driver operations table.
 *
 * @param netdev Pointer to the network device structure to register.
 * @return 0 on success, or a negative errno value on error.
 */
int ena_netdev_register(struct uk_netdev *netdev);

/**
 * Get the current link state from the AENQ LINK_CHANGE events.
 *
 * @param dev Pointer to the network device.
 * @return true when the link is up, false when it is down.
 */
bool ena_netdev_link_get(struct uk_netdev *dev);

#endif /* !__Unikraft__ */

#endif /* LIBENA_ENA_NETDEV_H */

