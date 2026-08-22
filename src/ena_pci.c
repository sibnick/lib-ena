/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"

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
 * Unikraft PCI driver registration.
 *
 * This block is only compiled under Unikraft (it is not host-testable).
 * The uk PCI API assumed here, kept to the minimal well-known set:
 *   - struct uk_pci_device              opaque PCI device handle
 *   - struct uk_pci_id                  id pair, fields { vendor, device }
 *   - struct uk_pci_driver              fields { .name, .ids, .probe, .remove }
 *   - void *uk_pci_map_bar(pdev, bar, uint32_t *size)   map a BAR
 *   - void uk_pci_unmap_bar(pdev, void *vaddr)          unmap a BAR
 *   - void uk_pci_set_drvdata(pdev, void *data)         attach private data
 *   - void *uk_pci_get_drvdata(pdev)                     read private data
 *   - UK_PCI_DRIVER_REGISTER(driver)                     register the driver
 */

#include <errno.h>
#include <uk/alloc.h>
#include <uk/bus/pci.h>

/* A bound ENA PCI device: owns the driver adapter and the uk device. */
struct ena_uk_device {
	struct ena_adapter adapter;
	struct uk_pci_device *pdev;
	void *bar0_vaddr;
	void *bar2_vaddr;
};

static int ena_uk_pci_probe(struct uk_pci_device *pdev)
{
	struct ena_uk_device *edev;
	void *bar0;
	uint32_t bar0_size = 0;
	void *bar2;
	uint32_t bar2_size = 0;
	int ret;

	edev = uk_malloc(uk_alloc_get_default(), sizeof(*edev));
	if (!edev) {
		ena_err("probe: allocation failed");
		return -ENOMEM;
	}

	memset(edev, 0, sizeof(*edev));
	edev->pdev = pdev;
	edev->bar0_vaddr = NULL;
	edev->bar2_vaddr = NULL;

	/* Map BAR0 (the MMIO register space). */
	bar0 = uk_pci_map_bar(pdev, 0, &bar0_size);
	if (!bar0) {
		ena_err("probe: BAR0 map failed");
		uk_free(uk_alloc_get_default(), edev);
		return -ENODEV;
	}
	edev->bar0_vaddr = bar0;

	/* Map BAR2 (LLQ push memory, optional on instances without LLQ). */
	bar2 = uk_pci_map_bar(pdev, 2, &bar2_size);
	if (bar2) {
		edev->bar2_vaddr = bar2;
		edev->adapter.bar2_base = bar2;
		edev->adapter.bar2_size = bar2_size;
	}

	/* Bind the device through the existing Phase 1 scaffold. */
	ret = ena_device_init_scaffold(&edev->adapter, bar0, bar0_size);
	if (ret) {
		ena_err("probe: init scaffold failed (%d)", ret);
		if (edev->bar2_vaddr)
			uk_pci_unmap_bar(pdev, edev->bar2_vaddr);
		uk_pci_unmap_bar(pdev, bar0);
		uk_free(uk_alloc_get_default(), edev);
		return ret;
	}

	uk_pci_set_drvdata(pdev, edev);

	ena_info("probe: bound ENA device (bar0=%p, size=%u, bar2=%p, size=%u)",
		 bar0, bar0_size, bar2, bar2_size);
	return 0;
}

static void ena_uk_pci_remove(struct uk_pci_device *pdev)
{
	struct ena_uk_device *edev;

	edev = uk_pci_get_drvdata(pdev);
	if (!edev)
		return;

	/* Reset device and wait for completion, then release BAR mappings. */
	ena_device_reset(&edev->adapter);
	ena_device_wait_reset_complete(&edev->adapter, 100);
	ena_admin_fini(&edev->adapter);
	if (edev->bar2_vaddr)
		uk_pci_unmap_bar(pdev, edev->bar2_vaddr);
	uk_pci_unmap_bar(pdev, edev->bar0_vaddr);
	uk_pci_set_drvdata(pdev, NULL);
	uk_free(uk_alloc_get_default(), edev);
}

/* The same five vendor/device pairs as ena_pci_match_id() above. */
static const struct uk_pci_id ena_uk_pci_ids[] = {
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_RESERVED },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_PF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_PF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_VF },
	{ ENA_PCI_VENDOR_ID, ENA_PCI_DEV_ID_LLQ_VF },
	{ 0, 0 }
};

struct uk_pci_driver ena_uk_pci_driver = {
	.name = "ena",
	.ids = ena_uk_pci_ids,
	.probe = ena_uk_pci_probe,
	.remove = ena_uk_pci_remove,
};

UK_PCI_DRIVER_REGISTER(ena_uk_pci_driver);

#endif /* __Unikraft__ */
