include ../common.mk
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run: test
	./test
clean:
	rm -f test
.PHONY: run clean
