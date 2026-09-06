/*
 * Copyright (C) 2024-2026 the original author or authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * httpreply-mc: multi-core HTTP reply benchmark server.
 *
 * Skeleton entry point (ticket 3f9034ff15). The full multi-core
 * accept / distribute / per-worker HTTP logic is added in later
 * tickets. It uses threaded lwIP (one dispatcher thread per RX queue)
 * and per-core worker threads on a 2-vCPU KVM target with the ENA
 * driver in 2-queue-pair mode.
 */

#include <stdio.h>

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("httpreply-mc: multi-core HTTP reply benchmark server\n");
	printf("httpreply-mc: mode=multi-core (threaded lwIP, 2 vCPU)\n");
	printf("httpreply-mc: queues=2 RX / 2 TX (target), workers=2\n");
	printf("httpreply-mc: skeleton build - core logic pending\n");

	return 0;
}
