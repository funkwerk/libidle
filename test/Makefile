CC ?= gcc
CFLAGS += -g -Wall -Werror -pthread

TESTS=accept receive sem_wait

default: ${TESTS}

${TESTS}: %: %.c
	$(CC) $(CFLAGS) -fPIC $^ -o $@

clean:
	rm ${TESTS} || true
