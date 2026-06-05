# Makefile for x710_check
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

all: $(BUILD_DIR)/x710_check

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/x710_check: x710_check.c | $(BUILD_DIR)
	$(CC) $< -o $@ $(CFLAGS) $(LDFLAGS)
	@echo ">>> 编译完成: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo ">>> 清理完成"

run: $(BUILD_DIR)/x710_check
	sudo $(BUILD_DIR)/x710_check -l 0 -n 4 --
