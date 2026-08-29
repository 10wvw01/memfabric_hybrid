include ../common.mk
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS) -lrt
run: test
	timeout 60 mpirun --allow-run-as-root -np 2 ./test
clean:
	rm -f test
.PHONY: run clean
