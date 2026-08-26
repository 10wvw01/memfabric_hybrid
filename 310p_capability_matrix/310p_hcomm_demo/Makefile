include ../common.mk
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
test_cross_ipc: test_cross_ipc.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run: test
	timeout 120 mpirun --allow-run-as-root -np 8 ./test
run_cross: test_cross_ipc
	timeout 60 mpirun --allow-run-as-root -np 2 ./test_cross_ipc
clean:
	rm -f test test_cross_ipc
.PHONY: run run_cross clean
