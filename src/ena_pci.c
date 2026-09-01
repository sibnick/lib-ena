/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"
#include "ena_netdev.h"

/* Supported ENA PCI device IDs (from reference/ena_pci_id_tbl.h) */
static const struct ena_pci_id supported_pci_ids[] = {
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_RESERVED },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_PF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_PF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_VF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_VF },
};

int ena_pci_match_id(uint16_t vendor_id, uint16_t device_id)
{
	size_t num_ids = sizeof(supported_pci_ids) / sizeof(supported_pci_ids[0]);
	for (size_t i = 0; i < num_ids; i++) {
		if (supported_pci_ids[i].vendor_id == vendor_id &&
		    supported_pci_ids[i].device_id == device_id) {
			return 1;
		}
	}
	return 0;
}

#ifdef __Unikraft__

/*
 * Unikraft PCI driver registration and initialization.
 */

#include <errno.h>
#include <uk/alloc.h>
#include <uk/arch/util.h>
#include <uk/bus/pci.h>
#include <uk/netdev_driver.h>

#include "ena_intr.h"

static inline uint32_t pci_read32(const struct pci_address *addr, uint32_t reg)
{
	uint32_t config_addr = (1u << 31)
		| ((uint32_t)addr->bus << 16)
		| ((uint32_t)addr->devid << 11)
		| ((uint32_t)addr->function << 8)
		| (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	return uk_arch_x86_64_inl(PCI_CONFIG_DATA);
}

static inline void pci_write32(const struct pci_address *addr, uint32_t reg, uint32_t val)
{
	uint32_t config_addr = (1u << 31)
		| ((uint32_t)addr->bus << 16)
		| ((uint32_t)addr->devid << 11)
		| ((uint32_t)addr->function << 8)
		| (reg & 0xFC);
	uk_arch_x86_64_outl(PCI_CONFIG_ADDR, config_addr);
	uk_arch_x86_64_outl(PCI_CONFIG_DATA, val);
}

static inline void pci_enable_device(const struct pci_address *addr)
{
	uint32_t cmd = pci_read32(addr, 0x04);
	cmd |= (1u << 0)  /* I/O Space Enable */
	     | (1u << 1)  /* Memory Space Enable */
	     | (1u << 2); /* Bus Master Enable (DMA) */
	pci_write32(addr, 0x04, cmd);
}

static inline uint64_t pci_read_bar0(const struct pci_address *addr)
{
	uint32_t bar0_lo = pci_read32(addr, 0x10);
	uint64_t bar0 = (bar0_lo & ~0x0Fu);
	if ((bar0_lo & 0x06) == 0x04) { /* 64-bit MMIO BAR */
		uint32_t bar0_hi = pci_read32(addr, 0x14);
		bar0 |= ((uint64_t)bar0_hi << 32);
	}
	return bar0;
}

static inline uint32_t pci_read_bar0_size(const struct pci_address *addr)
{
	uint32_t bar0_orig = pci_read32(addr, 0x10);
	pci_write32(addr, 0x10, 0xFFFFFFFFu);
	uint32_t mask = pci_read32(addr, 0x10);
	pci_write32(addr, 0x10, bar0_orig);

	uint32_t size_mask = mask & ~0x0Fu;
	if (size_mask == 0)
		return 0x4000;

	return (~size_mask) + 1;
}

static const struct pci_device_id ena_pci_ids[] = {
	{ PCI_DEVICE_ID(ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_RESERVED) },
	{ PCI_DEVICE_ID(ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_PF) },
	{ PCI_DEVICE_ID(ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_PF) },
	{ PCI_DEVICE_ID(ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_VF) },
	{ PCI_DEVICE_ID(ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_VF) },
	{ PCI_ANY_DEVICE_ID }
};

static int ena_pci_add_dev(struct pci_device *pdev)
{
	struct ena_uk_device *edev;
	void *bar0;
	uint64_t bar0_phys;
	uint32_t bar0_size;
	uint32_t sts;
	int ret;

	/* 1. Enable PCI Memory Space & Bus Mastering (DMA) */
	pci_enable_device(&pdev->addr);

	for (int b = 0; b < 6; b++) {
		uint32_t bar_val = pci_read32(&pdev->addr, 0x10 + b * 4);
		ena_info("PCI BAR%d = 0x%08x", b, bar_val);
	}

	/* 2. Read full 64-bit BAR0 MMIO address and size */
	bar0_size = pci_read_bar0_size(&pdev->addr);
	if (bar0_size < 0x104)
		bar0_size = 0x104;

	bar0_phys = pci_read_bar0(&pdev->addr);
	bar0 = (void *)(uintptr_t)bar0_phys;

	edev = uk_calloc(uk_alloc_get_default(), 1, sizeof(*edev));
	if (!edev) {
		ena_err("probe: allocation failed");
		return -ENOMEM;
	}

	edev->bar0_vaddr = bar0;

	/* 3. Initialize device scaffold */
	ret = ena_device_init_scaffold(&edev->adapter, bar0, bar0_size);
	if (ret) {
		ena_err("probe: init scaffold failed (%d)", ret);
		uk_free(uk_alloc_get_default(), edev);
		return ret;
	}

	sts = ena_reg_read32(edev->adapter.bar0_base + ENA_REGS_DEV_STS_OFF);
	ena_info("probe: bar0=%p (phys=0x%lx, size=0x%x) dev_sts=0x%x version=0x%x caps=0x%x",
		 bar0, (unsigned long)bar0_phys, bar0_size, sts,
		 edev->adapter.version, edev->adapter.caps);

	/* 4. Initialize Admin Queue & AENQ */
	ret = ena_admin_init(&edev->adapter, 32, 32, 32);
	if (ret) {
		ena_err("probe: admin init failed (%d)", ret);
		uk_free(uk_alloc_get_default(), edev);
		return ret;
	}

	/* 5. Run feature negotiation (device attributes, queue limits, host info, MTU, MAC) */
	ret = ena_init_run(&edev->adapter, 1500);
	if (ret) {
		ena_err("probe: init run failed (%d)", ret);
		ena_admin_fini(&edev->adapter);
		uk_free(uk_alloc_get_default(), edev);
		return ret;
	}

	/* 6. Register the default AENQ handler. It resets the device on a
	 *    FATAL_ERROR event and updates the link state on LINK_CHANGE. */
	ret = ena_admin_aenq_register(&edev->adapter, ena_aenq_default_handler,
				      &edev->adapter);
	if (ret)
		ena_warn("probe: aenq handler register failed (%d)", ret);

	/* 7. Allocate MSI-X vectors when the platform provides them.
	 *    Otherwise the driver stays in software polling mode. */
	ret = ena_intr_setup(&edev->adapter, &pdev->addr);
	if (ret < 0)
		ena_info("probe: msix inactive (%d), software polling mode", ret);

	edev->netdev.rx_one = ena_netdev_rx_one;
	edev->netdev.tx_one = ena_netdev_tx_one;
	edev->netdev.ops = &ena_ops;

	ret = uk_netdev_drv_register(&edev->netdev, uk_alloc_get_default(), "ena");
	if (ret < 0) {
		ena_err("probe: failed to register uknetdev (%d)", ret);
		ena_intr_msix_fini(&edev->adapter);
		ena_admin_fini(&edev->adapter);
		uk_free(uk_alloc_get_default(), edev);
		return ret;
	}

	edev->uid = (uint16_t)ret;

	ena_info("probe: bound ENA device netdev_id=%u (mac=%02x:%02x:%02x:%02x:%02x:%02x)",
		 edev->uid,
		 edev->adapter.mac_addr[0], edev->adapter.mac_addr[1],
		 edev->adapter.mac_addr[2], edev->adapter.mac_addr[3],
		 edev->adapter.mac_addr[4], edev->adapter.mac_addr[5]);
	return 0;
}

static struct pci_driver ena_pci_drv = {
	.device_ids = ena_pci_ids,
	.add_dev = ena_pci_add_dev,
};

PCI_REGISTER_DRIVER(&ena_pci_drv);

#endif /* __Unikraft__ */
