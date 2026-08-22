# SPDX-License-Identifier: BSD-3-Clause
# Standalone build & test harness for Unikraft ENA driver

CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude -Ireference -Itests -D_POSIX_C_SOURCE=200809L

BUILD = build
TEST1 = $(BUILD)/test_runner
TEST2 = $(BUILD)/test_admin
TEST3 = $(BUILD)/test_init
TEST4 = $(BUILD)/test_datapath
TEST5 = $(BUILD)/test_tx
TEST6 = $(BUILD)/test_rx
TEST7 = $(BUILD)/test_netdev
TEST8 = $(BUILD)/test_intr
TEST9 = $(BUILD)/test_llq
TEST10 = $(BUILD)/test_validation

ENA_SRCS = src/ena_pci.c src/ena_com.c src/ena_plat.c
ENA_SRCS_P2 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c
ENA_SRCS_P3 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c
ENA_SRCS_P4 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c
ENA_SRCS_P5 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c
ENA_SRCS_P6 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c
ENA_SRCS_P7 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_netdev.c
ENA_SRCS_P8 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_intr.c
ENA_SRCS_P9 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_intr.c src/ena_llq.c
ENA_SRCS_ALL = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_netdev.c src/ena_intr.c src/ena_llq.c
ENA_HDRS = include/ena.h include/ena_regs.h include/ena_plat.h include/ena_admin.h include/ena_init.h include/ena_datapath.h include/ena_netdev.h include/ena_intr.h include/ena_llq.h

.PHONY: all test sanitize clean

all: test

sanitize: CFLAGS += -fsanitize=address -g
sanitize: clean test

test: $(TEST1) $(TEST2) $(TEST3) $(TEST4) $(TEST5) $(TEST6) $(TEST7) $(TEST8) $(TEST9) $(TEST10)
	./$(TEST1)
	./$(TEST2)
	./$(TEST3)
	./$(TEST4)
	./$(TEST5)
	./$(TEST6)
	./$(TEST7)
	./$(TEST8)
	./$(TEST9)
	./$(TEST10)

$(TEST1): tests/test_pci_scaffold.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_pci_scaffold.c tests/mock_pci.c $(ENA_SRCS)

$(TEST2): tests/test_admin_queue.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P2) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_admin_queue.c tests/mock_pci.c $(ENA_SRCS_P2) -pthread

$(TEST3): tests/test_init.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P3) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_init.c tests/mock_pci.c $(ENA_SRCS_P3)

$(TEST4): tests/test_datapath_rings.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P4) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_datapath_rings.c tests/mock_pci.c $(ENA_SRCS_P4)

$(TEST5): tests/test_tx_datapath.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P5) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_tx_datapath.c tests/mock_pci.c $(ENA_SRCS_P5)

$(TEST6): tests/test_rx_datapath.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P6) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_rx_datapath.c tests/mock_pci.c $(ENA_SRCS_P6)

$(TEST7): tests/test_netdev.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P7) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_netdev.c tests/mock_pci.c $(ENA_SRCS_P7)

$(TEST8): tests/test_intr.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P8) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_intr.c tests/mock_pci.c $(ENA_SRCS_P8)

$(TEST9): tests/test_llq.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_P9) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_llq.c tests/mock_pci.c $(ENA_SRCS_P9)

$(TEST10): tests/test_validation.c tests/mock_pci.c tests/mock_pci.h $(ENA_SRCS_ALL) $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_validation.c tests/mock_pci.c $(ENA_SRCS_ALL)

clean:
	rm -rf $(BUILD)
