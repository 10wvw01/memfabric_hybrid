include ../../common.mk

all: minitest minitest3 minitest6 minitest8 minitest8b

minitest: minitest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

minitest3: minitest3.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

minitest6: minitest6.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

minitest8: minitest8.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

minitest8b: minitest8b.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f minitest minitest3 minitest6 minitest8 minitest8b

.PHONY: all clean
