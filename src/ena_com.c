/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#include "ena.h"

#include <errno.h>
#include <string.h>

/* Minimum BAR0 size to hold all defined registers (PHC_DB at offset 0x100). */
#define ENA_BAR0_MIN_SIZE	0x104

#ifndef __Unikraft__
static ena_reset_poll_hook *s_reset_poll_hook;
static void *s_reset_poll_cookie;

void ena_device_set_reset_poll_hook(ena_reset_poll_hook *hook, void *cookie)
{
	s_reset_poll_hook = hook;
	s_reset_poll_cookie = cookie;
}
#endif

int ena_device_check_ready(const struct ena_adapter *adapter)
{
	if (!adapter || !adapter->bar0_base)
		return -EINVAL;

	uint32_t status = ena_reg_read32(adapter->bar0_base + ENA_REGS_DEV_STS_OFF);
	if (!(status & ENA_DEV_STS_READY_MASK))
		return -EBUSY;

	return 0;
}

int ena_device_reset(struct ena_adapter *adapter)
{
	if (!adapter || !adapter->bar0_base)
		return -EINVAL;

	/* Read-modify-write reset bit and reason code, preserving other DEV_CTL bits. */
	uint32_t ctl = ena_reg_read32(adapter->bar0_base + ENA_REGS_DEV_CTL_OFF);
	ctl &= ~ENA_DEV_CTL_RESET_REASON_MASK;
	ctl |= (1u << ENA_DEV_CTL_RESET_REASON_SHIFT);
	ctl |= ENA_DEV_CTL_DEV_RESET_MASK;
	ena_reg_write32(adapter->bar0_base + ENA_REGS_DEV_CTL_OFF, ctl);

	return 0;
}

int ena_device_wait_reset_complete(struct ena_adapter *adapter, unsigned int max_polls)
{
	if (!adapter || !adapter->bar0_base)
		return -EINVAL;
	if (max_polls == 0)
		max_polls = 1;

	/* Bounded poll for reset completion (or a fatal error). A short
	 * delay between reads makes the budget time-based, so a real reset
	 * (milliseconds) is not exhausted by a tight spin. */
	for (unsigned int i = 0; i < max_polls; i++) {
		uint32_t status = ena_reg_read32(adapter->bar0_base + ENA_REGS_DEV_STS_OFF);
		if (status & ENA_DEV_STS_FATAL_ERROR_MASK) {
			adapter->state = ENA_STATE_ERROR;
			return -EIO;
		}
		/* Complete only when the finished flag is set and the
		 * in-progress flag has cleared. */
		if ((status & ENA_DEV_STS_RESET_FIN_MASK) &&
		    !(status & ENA_DEV_STS_RESET_IN_PROG_MASK))
			return 0;
#ifndef __Unikraft__
		if (s_reset_poll_hook)
			s_reset_poll_hook(s_reset_poll_cookie);
#endif
		ena_delay_us(100);
	}

	return -ETIMEDOUT;
}

int ena_device_init_scaffold(struct ena_adapter *adapter, void *bar0_base, size_t bar0_size)
{
	if (!adapter || !bar0_base || bar0_size < ENA_BAR0_MIN_SIZE)
		return -EINVAL;

	memset(adapter, 0, sizeof(*adapter));
	adapter->bar0_base = (volatile uint8_t *)bar0_base;
	adapter->bar0_size = bar0_size;
	adapter->state = ENA_STATE_PCI_PROBED;

	adapter->version = ena_reg_read32(adapter->bar0_base + ENA_REGS_VERSION_OFF);
	adapter->controller_version = ena_reg_read32(adapter->bar0_base + ENA_REGS_CONTROLLER_VERSION_OFF);
	adapter->caps = ena_reg_read32(adapter->bar0_base + ENA_REGS_CAPS_OFF);

	return 0;
}
