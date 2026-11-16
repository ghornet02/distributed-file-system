CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

all: nameserver client

nameserver: src/nameserver.c
	$(CC) $(CFLAGS) -o $@ $<

client: src/client.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nameserver client

.PHONY: all clean
