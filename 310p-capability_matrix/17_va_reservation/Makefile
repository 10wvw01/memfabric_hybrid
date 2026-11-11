ASCEND_HOME ?= $(if $(ASCEND_HOME_PATH),$(ASCEND_HOME_PATH),/usr/local/Ascend/cann)
include ../common.mk
CFLAGS += -I$(ASCEND_HOME)/include -Wextra

all: test
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run:
	bash run.sh
clean:
	rm -f test
.PHONY: all run clean
