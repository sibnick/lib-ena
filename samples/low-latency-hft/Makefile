# Makefile for Low-Latency UDP Echo Server & Benchmark Client

CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -D_GNU_SOURCE -march=native
LDFLAGS ?= -pthread
LDLIBS ?= -lm

BIN_DIR = bin
SERVER_BIN = $(BIN_DIR)/server
CLIENT_BIN = $(BIN_DIR)/client

.PHONY: all clean test benchmark

all: $(SERVER_BIN) $(CLIENT_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SERVER_BIN): server.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(CLIENT_BIN): client.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)

test: all
	@echo "Starting UDP echo test on port 9876..."
	@$(SERVER_BIN) -p 9876 > /dev/null 2>&1 & SERVER_PID=$$!; \
	sleep 0.1; \
	$(CLIENT_BIN) -p 9876 -n 5000; \
	CLIENT_STATUS=$$?; \
	kill -TERM $$SERVER_PID 2>/dev/null || true; \
	sleep 0.1; \
	kill -KILL $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	exit $$CLIENT_STATUS

benchmark: all
	@echo "Running unthrottled benchmark (50,000 packets)..."
	@$(SERVER_BIN) -p 9876 > /dev/null 2>&1 & SERVER_PID=$$!; \
	sleep 0.1; \
	$(CLIENT_BIN) -p 9876 -n 50000; \
	CLIENT_STATUS=$$?; \
	kill -TERM $$SERVER_PID 2>/dev/null || true; \
	sleep 0.1; \
	kill -KILL $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	exit $$CLIENT_STATUS
