CC ?= gcc
CFLAGS += -g -Wall -Werror -pthread

TESTS=accept receive sem_wait sem_post

default: ${TESTS}

${TESTS}: %: %.c
	$(CC) $(CFLAGS) -fPIC $^ -o $@

clean:
	rm ${TESTS} || true
