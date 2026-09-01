# Makefile for Low-Latency Transport (Dlang betterC + Harness)

LDC2 ?= ldc2
DFLAGS = -betterC -O3 -release -mcpu=native -I=src/common -I=src/network

SRCS_COMMON = src/common/wire_protocol.d src/common/shm_client.d src/common/fec.d src/common/sha256.d src/common/preflight_check.d src/network/socket.d

.PHONY: all harness clean test benchmark

all: harness bin/sender bin/receiver

harness:
	@echo "Building harness..."
	$(MAKE) -C harness

bin/sender: src/sender_main.d $(SRCS_COMMON)
	@mkdir -p bin
	@echo "Compiling bin/sender..."
	$(LDC2) $(DFLAGS) $(SRCS_COMMON) src/sender_main.d -of=$@

bin/receiver: src/receiver_main.d $(SRCS_COMMON)
	@mkdir -p bin
	@echo "Compiling bin/receiver..."
	$(LDC2) $(DFLAGS) $(SRCS_COMMON) src/receiver_main.d -of=$@

clean:
	@echo "Cleaning binaries..."
	rm -rf bin build
	$(MAKE) -C harness clean

test: all
	@echo "Running harness assertion tests..."
	$(MAKE) -C harness test

benchmark: all
	@echo "Running local benchmark test..."
	./harness/bin/producer --shm /test_prod_ring --slots 1024 --count 100000 --rate 200000 --type mixed &
	sleep 0.05
	./bin/sender --shm /test_prod_ring --dest 127.0.0.1 --port 9000 &
	./bin/receiver --shm /test_cons_ring --port 9000 &
	sleep 0.05
	./harness/bin/consumer --shm /test_cons_ring --count 100000 --idle-ms 1000
