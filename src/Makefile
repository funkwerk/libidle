CC ?= gcc

libidle.so: *.c
	$(CC) -g -fPIC -Wall -Werror -ldl -shared -Wl,--version-script -Wl,libidle.map -o $@ $^

clean:
	rm libidle.so
