CC ?= gcc

libidle.so: *.c
	$(CC) -g -fPIC -ldl -shared -o $@ $^

clean:
	rm libidle.so
