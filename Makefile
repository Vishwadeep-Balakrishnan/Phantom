CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -march=native \
           -Iinclude -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

SRC_DIR   = src
BUILD_DIR = build
BENCH_DIR = bench

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TARGET       = phantom
BENCH_TARGET = bench/bench
CLI_TARGET   = bench/phantom-cli

.PHONY: all clean cluster bench fmt

all: $(BUILD_DIR) $(TARGET) $(BENCH_TARGET) $(CLI_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BENCH_TARGET): $(BENCH_DIR)/bench.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

$(CLI_TARGET): $(BENCH_DIR)/client.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

# Spin up a 3-node cluster locally.
# Node 1 is the first to start and acts as the seed for nodes 2 and 3.
cluster: all
	@echo "Starting 3-node phantom cluster..."
	@mkdir -p data/node-1 data/node-2 data/node-3
	@./phantom 1 7600 7700 &
	@sleep 0.3
	@./phantom 2 7601 7701 127.0.0.1:7700:1 &
	@sleep 0.3
	@./phantom 3 7602 7702 127.0.0.1:7700:1 &
	@echo ""
	@echo "Cluster running:"
	@echo "  node 1: client=7600 gossip=7700"
	@echo "  node 2: client=7601 gossip=7701"
	@echo "  node 3: client=7602 gossip=7702"
	@echo ""
	@echo "Try: ./bench/phantom-cli 127.0.0.1 7600"
	@echo "     kill one node and watch gossip detect it."
	@echo "     Press Ctrl-C to stop all nodes."
	@wait

stop-cluster:
	@pkill -f './phantom [0-9]' || true

bench-run: all
	@echo "Running benchmark against node 1 (make sure cluster is up)..."
	./bench/bench 127.0.0.1 7600 8 10000

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(BENCH_TARGET) $(CLI_TARGET) data/

fmt:
	@command -v clang-format >/dev/null && \
	  find src include bench -name '*.c' -o -name '*.h' | xargs clang-format -i || \
	  echo "clang-format not found, skipping"
