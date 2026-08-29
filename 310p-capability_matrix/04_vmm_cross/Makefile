include ../common.mk
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run: test
	mpirun --allow-run-as-root -np 2 ./test
clean:
	rm -f test
.PHONY: run clean
