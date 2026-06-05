# Makefile for rss_verify
#
# 用法：
#   make        编译
#   make run    运行
#   make clean  清理

CC      := gcc
CFLAGS  := $(shell pkg-config --cflags libdpdk) -Wall -O2
LDFLAGS := $(shell pkg-config --libs libdpdk) \
           -Wl,--whole-archive \
           -lrte_net_i40e \
           -Wl,--no-whole-archive

BUILD_DIR := build

.PHONY: all clean run

all: $(BUILD_DIR)/rss_verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/rss_verify: rss_verify.c | $(BUILD_DIR)
	$(CC) $< -o $@ $(CFLAGS) $(LDFLAGS)
	@echo ">>> 编译完成: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo ">>> 清理完成"

run: $(BUILD_DIR)/rss_verify
	sudo $(BUILD_DIR)/rss_verify -l 0-4 -n 4 --