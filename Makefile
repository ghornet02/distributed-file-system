CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -std=c11 -D_DEFAULT_SOURCE
BINS = nameserver storageserver client

all: $(BINS)

%: src/%.c src/common.c src/common.h
	$(CC) $(CFLAGS) -o $@ $< src/common.c

test: all
	./tests/e2e.sh

clean:
	rm -f $(BINS)
	rm -rf tests/tmp

.PHONY: all test clean
