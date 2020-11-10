CC ?= gcc

libidle.so: *.c
	$(CC) -g -fPIC -Wall -Werror -ldl -shared -o $@ $^

clean:
	rm libidle.so
