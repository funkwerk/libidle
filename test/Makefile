CC ?= gcc
CFLAGS += -g -Wall -Werror -pthread

TESTS=accept sem_wait sem_post pthread_cond_signal

default: ${TESTS}

${TESTS}: %: %.c | build
	$(CC) $(CFLAGS) -fPIC $^ -o build/$@

build:
	mkdir build

clean:
	rm -rf build || true
