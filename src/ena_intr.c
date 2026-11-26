/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_intr.h"
#include "ena_datapath.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __Unikraft__
/*
 * The platform layer keeps a static copy of the arm request,
 * so this callback can keep the adapter in its argument.
 */
static void msix_on_fire(void *arg, uint32_t vector_id)
{
	struct ena_adapter *adapter = arg;

	if (vector_id >= adapter->num_irq_vectors)
		return;

	adapter->irq_vectors[vector_id].intr_count++;

	/*
	 * The device masks a queue interrupt after delivery. Write
	 * the queue unmask register here so the next completion
	 * fires again. The polling core handles the packets after
	 * the wake; re-arming in the handler keeps the device
	 * interrupt-driven.
	 */
	if (!adapter->irq_vectors[vector_id].is_admin)
		ena_intr_unmask_vector(adapter, vector_id);
}

/*
 * Arm the device MSI-X capability. The driver allocates one
 * unikernel vector per MSI-X vector. It targets each vector at
 * the core that polls the queue that vector reports.
 * CONFIG_LIBENA_MSIX_LCPU_LIST gives that core per queue. An empty
 * list keeps the old rule: queue q targets lcpu q % booted CPU
 * count. The driver then unmasks the capability. [Ticket 344310cd80]
 */
static int msix_arm_device(struct ena_adapter *adapter,
			   const struct pci_address *bdf)
{
	struct ena_msix_req req;
	uint32_t nvec;
	uint32_t i;
	uint32_t ncpus;
	char map[384];
	int used;
	int room;
	int n;
	int tcount;
	int ret;

	nvec = adapter->num_irq_vectors;
	if (nvec == 0)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.pci_dev = bdf;
	req.nvec = nvec;
	req.on_fire = msix_on_fire;
	req.arg = adapter;

/*
 * Take the lcpu target count from the configured max core
 * count. Use one core when the count is absent.
 */
#if defined(CONFIG_UKPLAT_CPU_MAXCOUNT) && (CONFIG_UKPLAT_CPU_MAXCOUNT > 0)
	ncpus = (uint32_t)CONFIG_UKPLAT_CPU_MAXCOUNT;
#else
	ncpus = 1;
#endif
	if (ncpus < 1)
		ncpus = 1;

#if defined(CONFIG_LIBENA_MSIX_LCPU_LIST)
	tcount = ena_intr_msix_targets(nvec, CONFIG_LIBENA_MSIX_LCPU_LIST,
				       ncpus, req.lcpu);
	if (tcount < 0) {
		ena_warn("msix: bad lcpu list '%s' (%d), using modulo targets",
			 CONFIG_LIBENA_MSIX_LCPU_LIST, tcount);
		tcount = ena_intr_msix_targets(nvec, "", ncpus, req.lcpu);
	}
#else
	tcount = ena_intr_msix_targets(nvec, "", ncpus, req.lcpu);
#endif

	/* Print the vector-to-lcpu map once. The AWS fact run reads
	 * this line as placement evidence. [Ticket 344310cd80] */
	used =
	    snprintf(map, sizeof(map),
		     "msix: target map (%s):", tcount > 0 ? "list" : "modulo");
	if (used < 0)
		used = 0;
	room = (int)sizeof(map) - used;
	if (room < 0)
		room = 0;
	for (i = 0; i < nvec && room > 16; i++) {
		n = snprintf(map + used, room, " v%u->cpu%u", (unsigned)i,
			     (unsigned)req.lcpu[i]);
		if (n < 0 || n >= room)
			break;
		used += n;
		room -= n;
	}
	ena_info("%s", map);

	ret = ena_plat_msix_arm(&req);
	if (ret)
		ena_err("msix: device arm failed (%d)", ret);
	return ret;
}
#endif /* __Unikraft__ */

/*
 * Compute the MSI-X lcpu target list. See the contract comment in
 * include/ena_intr.h. This function is pure, so the host test suite
 * checks it without a device. [Ticket 344310cd80]
 */
int ena_intr_msix_targets(uint32_t nvec, const char *lcpu_list, uint32_t ncpus,
			  uint32_t *out_lcpu)
{
	uint32_t targets[ENA_MAX_MSIX_VECTORS];
	uint32_t count = 0;
	const char *p;
	uint32_t q;
	char *end;
	unsigned long val;

	if (!out_lcpu || nvec == 0 || nvec > ENA_MAX_MSIX_VECTORS)
		return -EINVAL;
	if (ncpus < 1)
		ncpus = 1;

	/* Vector 0 is the Admin/AENQ vector. It targets lcpu 0. */
	out_lcpu[0] = 0;

	if (!lcpu_list || lcpu_list[0] == '\0') {
		for (q = 1; q < nvec; q++)
			out_lcpu[q] = (q - 1) % ncpus;
		return 0;
	}

	p = lcpu_list;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p < '0' || *p > '9')
			return -EINVAL;
		val = strtoul(p, &end, 10);
		if (end == p || val >= (unsigned long)ncpus)
			return -EINVAL;
		if (count >= ENA_MAX_MSIX_VECTORS)
			return -EINVAL;
		targets[count++] = (uint32_t)val;
		p = end;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		if (*p != ',')
			return -EINVAL;
		p++;
	}

	for (q = 1; q < nvec; q++)
		out_lcpu[q] = targets[(q - 1) % count];
	return (int)count;
}

int ena_intr_msix_init(struct ena_adapter *adapter, uint32_t num_vectors)
{
	uint32_t i;

	if (!adapter || num_vectors == 0 || num_vectors > ENA_MAX_MSIX_VECTORS)
		return -EINVAL;

	adapter->irq_vectors =
	    calloc(num_vectors, sizeof(*adapter->irq_vectors));
	if (!adapter->irq_vectors)
		return -ENOMEM;

	adapter->num_irq_vectors = num_vectors;

	/* Vector 0 is dedicated to Admin/AENQ management events */
	adapter->irq_vectors[0].vector_id = 0;
	adapter->irq_vectors[0].is_admin = true;
	adapter->irq_vectors[0].masked = true;
	adapter->irq_vectors[0].moderation_interval_usec = 0;

	/* Vectors 1..N-1 are dedicated to IO queues */
	for (i = 1; i < num_vectors; i++) {
		adapter->irq_vectors[i].vector_id = i;
		adapter->irq_vectors[i].queue_id = (uint16_t)(i - 1);
		adapter->irq_vectors[i].is_admin = false;
		adapter->irq_vectors[i].masked = true;
		adapter->irq_vectors[i].moderation_interval_usec = 20;
	}

	return 0;
}

void ena_intr_msix_fini(struct ena_adapter *adapter)
{
	if (!adapter || !adapter->irq_vectors)
		return;

#ifdef __Unikraft__
	/* Release any armed platform vectors before the table goes. */
	ena_plat_msix_disarm();
#endif

	ena_intr_mask_all(adapter);
	free(adapter->irq_vectors);
	adapter->irq_vectors = NULL;
	adapter->num_irq_vectors = 0;
}

int ena_intr_setup(struct ena_adapter *adapter, void *pci_dev)
{
	uint32_t nvec = 0;
	int ret;

	if (!adapter)
		return -EINVAL;

	if (adapter->irq_vectors)
		return 0; /* Already set up. */

	ret = ena_plat_msix_probe(pci_dev, &nvec);
	if (ret)
		return ret;

	if (nvec == 0) {
		ena_info(
		    "msix: no vectors from platform, using software polling");
		return -ENOTSUP;
	}

	if (nvec > ENA_MAX_MSIX_VECTORS)
		nvec = ENA_MAX_MSIX_VECTORS;

	ret = ena_intr_msix_init(adapter, nvec);
	if (ret) {
		ena_err("msix: vector table init failed (%d)", ret);
		return ret;
	}

	/* Enable the device interrupt for the admin/AENQ vector (vector 0).
	 * IO queue vectors stay masked until their queues are active. */
	ena_intr_unmask_vector(adapter, 0);

#ifdef __Unikraft__
#if defined(CONFIG_LIBENA_MSIX)
	/*
	 * Arm the real MSI-X capability (table, PBA, unmask). The
	 * platform pci_dev handle is the PCI bus/device/function. On
	 * failure the driver stays in software polling mode.
	 */
	{
		const struct pci_address *bdf =
		    (const struct pci_address *)pci_dev;

		ret = msix_arm_device(adapter, bdf);
		if (ret) {
			ena_warn(
			    "msix: arm failed (%d), using software polling",
			    ret);
			ena_intr_msix_fini(adapter);
			return ret;
		}
	}
#else
	/* Arming is off in this bisect build: software polling mode. */
	ena_info("msix: arming disabled (bisect build); software polling mode");
#endif
#endif

	ena_info("msix: %u vectors active (admin vector enabled)",
		 (unsigned)nvec);
	return 0;
}

int ena_intr_mask_vector(struct ena_adapter *adapter, uint32_t vector_id)
{
	if (!adapter || !adapter->irq_vectors ||
	    vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].masked = true;

	/* INTR_MASK register bit 0: 1 = masked, 0 = unmasked. */
	if (adapter->bar0_base && vector_id == 0)
		ena_reg_write32(adapter->bar0_base + ENA_REGS_INTR_MASK_OFF, 1);

	return 0;
}

int ena_intr_unmask_vector(struct ena_adapter *adapter, uint32_t vector_id)
{
	uint16_t num_tx;
	uint16_t num_rx;
	uint16_t qid;

	if (!adapter || !adapter->irq_vectors ||
	    vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].masked = false;

	if (!adapter->bar0_base)
		return 0;

	if (vector_id == 0) {
		ena_reg_write32(adapter->bar0_base + ENA_REGS_INTR_MASK_OFF, 0);
		return 0;
	}

	/* IO vector: unmask the queue interrupt via the dedicated
	 * register reported by the CREATE_CQ response. Writing
	 * intr_control bit 30 to that register unmasks the queue.
	 * The CQ head doorbell is not written. */
	qid = adapter->irq_vectors[vector_id].queue_id;
	num_tx = adapter->num_tx_rings ? adapter->num_tx_rings
				       : adapter->max_tx_queues;
	num_rx = adapter->num_rx_rings ? adapter->num_rx_rings
				       : adapter->max_rx_queues;
	if (adapter->rx_rings && qid < num_rx && adapter->rx_rings[qid] &&
	    adapter->rx_rings[qid]->cq_unmask_db_offset != 0) {
		ena_reg_write32(adapter->bar0_base +
				    adapter->rx_rings[qid]->cq_unmask_db_offset,
				ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
	}
	if (adapter->tx_rings && qid < num_tx && adapter->tx_rings[qid] &&
	    adapter->tx_rings[qid]->cq_unmask_db_offset != 0) {
		ena_reg_write32(adapter->bar0_base +
				    adapter->tx_rings[qid]->cq_unmask_db_offset,
				ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK);
	}

	return 0;
}

void ena_intr_mask_all(struct ena_adapter *adapter)
{
	uint32_t i;

	if (!adapter || !adapter->irq_vectors)
		return;

	for (i = 0; i < adapter->num_irq_vectors; i++)
		ena_intr_mask_vector(adapter, i);
}

void ena_intr_unmask_all(struct ena_adapter *adapter)
{
	uint32_t i;

	if (!adapter || !adapter->irq_vectors)
		return;

	for (i = 0; i < adapter->num_irq_vectors; i++)
		ena_intr_unmask_vector(adapter, i);
}

int ena_intr_set_coalesce(struct ena_adapter *adapter, uint32_t vector_id,
			  uint32_t usecs)
{
	if (!adapter || !adapter->irq_vectors ||
	    vector_id >= adapter->num_irq_vectors)
		return -EINVAL;

	adapter->irq_vectors[vector_id].moderation_interval_usec = usecs;
	return 0;
}

int ena_poll_step(struct ena_poll_ctx *ctx, unsigned int *work_done)
{
	struct ena_rx_pkt rx_pkts[32];
	unsigned int total = 0;
	unsigned int cleaned = 0;
	uint16_t num_tx;
	uint16_t num_rx;
	int count;
	int i;
	uint16_t q;

	if (!ctx || !ctx->adapter)
		return -EINVAL;

	if (ctx->tx_budget == 0)
		ctx->tx_budget = 32;
	if (ctx->rx_budget == 0)
		ctx->rx_budget = 32;

	num_tx = ctx->adapter->num_tx_rings;
	num_rx = ctx->adapter->num_rx_rings;
	if (num_tx == 0 && num_rx == 0) {
		if (work_done)
			*work_done = 0;
		return 0;
	}

	/* TX completion polling */
	if (ctx->adapter->tx_rings) {
		for (q = 0; q < num_tx; q++) {
			if (!ctx->adapter->tx_rings[q])
				continue;

			cleaned = 0;
			ena_tx_poll_completions(ctx->adapter->tx_rings[q],
						ctx->tx_budget, &cleaned);
			total += cleaned;
			ctx->total_tx_cleaned += cleaned;
		}
	}

	/* RX packet polling */
	if (ctx->adapter->rx_rings) {
		for (q = 0; q < num_rx; q++) {
			if (!ctx->adapter->rx_rings[q])
				continue;

			count = ena_rx_poll(
			    ctx->adapter->rx_rings[q], rx_pkts,
			    ctx->rx_budget > 32 ? 32 : ctx->rx_budget, NULL);
			if (count > 0) {
				for (i = 0; i < count; i++)
					if (ctx->rx_handler)
						ctx->rx_handler(ctx->rx_arg, q,
								&rx_pkts[i]);
				total += (unsigned int)count;
				ctx->total_rx_received += (uint64_t)count;
			}
		}
	}

	if (work_done)
		*work_done = total;

	return (int)total;
}
