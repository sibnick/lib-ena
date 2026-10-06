# SPDX-License-Identifier: BSD-3-Clause
# Standalone build & test harness for Unikraft ENA driver

CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Iinclude -Ireference -Itests -D_POSIX_C_SOURCE=200809L

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
TEST11 = $(BUILD)/test_spsc
TEST12 = $(BUILD)/test_idlebackoff
TEST13 = $(BUILD)/test_rss_skew
TEST14 = $(BUILD)/test_tx_guard

ENA_SRCS = src/ena_pci.c src/ena_com.c src/ena_plat.c
ENA_SRCS_P2 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c
ENA_SRCS_P3 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c
ENA_SRCS_P4 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c
ENA_SRCS_P5 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c
ENA_SRCS_P6 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c
ENA_SRCS_P7 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_intr.c src/ena_netdev.c src/ena_llq.c src/ena_rss.c
ENA_SRCS_P8 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_intr.c src/ena_rss.c
ENA_SRCS_P9 = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_intr.c src/ena_netdev.c src/ena_llq.c src/ena_rss.c
ENA_SRCS_ALL = src/ena_pci.c src/ena_com.c src/ena_admin.c src/ena_plat.c src/ena_init.c src/ena_datapath.c src/ena_tx.c src/ena_rx.c src/ena_netdev.c src/ena_intr.c src/ena_llq.c src/ena_rss.c
ENA_HDRS = include/ena.h include/ena_regs.h include/ena_plat.h include/ena_admin.h include/ena_init.h include/ena_datapath.h include/ena_netdev.h include/ena_intr.h include/ena_llq.h include/ena_rss.h

CLANG_FORMAT ?= clang-format
# Project-owned C sources. The prune list skips vendored and generated trees:
# Unikraft build output, fetched libraries, and the reference/ headers.
FORMAT_DIRS = src include tests samples examples
FORMAT_PRUNE = -name build -o -name .libs -o -name .unikraft -o -name .git
FORMAT_SRCS = $(shell find $(FORMAT_DIRS) \
	\( $(FORMAT_PRUNE) \) -prune -o \
	-type f \( -name '*.c' -o -name '*.h' \) -print | sort)

.PHONY: all test sanitize test-sanitize format format-check clean

all: test

sanitize: test-sanitize

# Keep the sanitizer build at -O0. Optimized code hides UB and shifts ASAN
# reports away from the faulting line. -O0 is last, so it wins over -O2.
test-sanitize: CFLAGS += -O0 -fsanitize=address,undefined -g
test-sanitize: clean test

test: $(TEST1) $(TEST2) $(TEST3) $(TEST4) $(TEST5) $(TEST6) $(TEST7) $(TEST8) $(TEST9) $(TEST10) $(TEST11) $(TEST12) $(TEST13) $(TEST14)
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
	./$(TEST11)
	./$(TEST12)
	./$(TEST13)
	./$(TEST14)

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

$(TEST11): tests/test_spsc.c samples/httpreply-mc/spsc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isamples/httpreply-mc -o $@ tests/test_spsc.c -pthread

$(TEST12): tests/test_idlebackoff.c samples/httpreply-mc/idlebackoff.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isamples/httpreply-mc -o $@ tests/test_idlebackoff.c

# Host analysis for ticket ca72834ec7. The RSS simulation models the
# Toeplitz hash and the round-robin indirection table from src/ena_rss.c.
# It asserts only its own hash self-checks, so it exits 0 on a normal run.
$(TEST13): tests/test_rss_skew.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_rss_skew.c

# Host micro-benchmark for the TX cross-CPU guard in src/ena_tx.c. It
# links the real host platform stub so ena_plat_cpu_id() is the driver's
# own function.
$(TEST14): tests/test_tx_guard.c src/ena_plat.c $(ENA_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_tx_guard.c src/ena_plat.c

# Rewrite the C sources in place with the style in .clang-format at the repo
# root. Aligned #define tables sit inside "clang-format off" guards, so the
# formatter keeps their column layout.
format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

# Fail when a source file does not match the style. Use this in CI.
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRCS)

clean:
	rm -rf $(BUILD)
