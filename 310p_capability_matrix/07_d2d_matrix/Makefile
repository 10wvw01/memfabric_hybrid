include ../common.mk
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run: test
	timeout 120 mpirun --allow-run-as-root -np 8 ./test
clean:
	rm -f test
.PHONY: run clean
