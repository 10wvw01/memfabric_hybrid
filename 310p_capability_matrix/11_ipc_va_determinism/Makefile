include ../common.mk

test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: test
	timeout 60 mpirun --allow-run-as-root -np 3 ./test

clean:
	rm -f test

.PHONY: run clean
