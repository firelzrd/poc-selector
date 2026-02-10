# SPDX-License-Identifier: GPL-2.0
#
# Scheduler tools
#

TARGETS := poc_bench

CC      ?= gcc
CFLAGS  += -O2 -Wall -Wextra -g -D_GNU_SOURCE
LDLIBS  += -lpthread -lm -lrt

all: $(TARGETS)

poc_bench: poc_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) $(TARGETS)

.PHONY: all clean
