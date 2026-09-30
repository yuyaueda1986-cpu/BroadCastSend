CC      ?= gcc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Isrc
LDFLAGS ?=
LDLIBS  ?=

LIB_SRCS := src/config.c src/payload.c src/cycle_sched.c src/sender.c src/stats.c src/timeutil.c
LIB_OBJS := $(LIB_SRCS:.c=.o)

BINS  := broadcast_send udp_recv_check
TESTS := tests/test_sched tests/test_config tests/test_payload

.PHONY: all test clean

all: $(BINS)

broadcast_send: src/main.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

udp_recv_check: tools/udp_recv_check.o src/payload.o src/config.o src/timeutil.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/%: tests/%.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TESTS) broadcast_send
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "== tests/test_cli.sh"; ./tests/test_cli.sh

clean:
	rm -f $(BINS) $(TESTS) src/*.o tools/*.o tests/*.o
