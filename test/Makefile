CC ?= gcc
CFLAGS += -g -Wall -Werror

default: accept receive

accept: accept.c
	$(CC) $(CFLAGS) -fPIC $^ -o $@

receive: receive.c
	$(CC) $(CFLAGS) -fPIC $^ -o $@

clean:
	rm accept receive
