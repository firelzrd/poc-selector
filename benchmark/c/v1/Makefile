# SPDX-License-Identifier: GPL-2.0
#
# POC Selector Microbenchmark
#
# Usage:
#   make            # build
#   make benchmark  # build & run (sudo recommended)
#   make clean
#

CC       ?= gcc
CFLAGS   ?= -O2 -Wall
LDLIBS   := -lpthread -lm
BUILDDIR := .

BENCH_BIN := $(BUILDDIR)/poc_bench
BENCH_SRC := $(BUILDDIR)/poc_bench.c

.DEFAULT_GOAL := all
.PHONY: all benchmark clean help

all: $(BENCH_BIN)

$(BENCH_BIN): $(BENCH_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

benchmark: $(BENCH_BIN)
	@echo '=== POC Selector Microbenchmark ==='
	@echo '(sysctl toggling requires root)'
	@echo ''
	$(BENCH_BIN)

clean:
	rm -f $(BENCH_BIN)

help:
	@echo 'Targets:'
	@echo '  all        - build poc_bench (default)'
	@echo '  benchmark  - build and run all benchmarks'
	@echo '  clean      - remove built binaries'
	@echo ''
	@echo 'Options:'
	@echo '  CC=clang   - use alternative compiler'
	@echo ''
	@echo 'Examples:'
	@echo '  make'
	@echo '  sudo make benchmark'
