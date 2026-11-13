ASCEND_HOME ?= $(if $(ASCEND_HOME_PATH),$(ASCEND_HOME_PATH),/usr/local/Ascend/cann)
include ../common.mk
CFLAGS += -I$(ASCEND_HOME)/include -Wextra

all: test hal_vmm
test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
hal_vmm: hal_vmm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
run:
	bash run.sh
clean:
	rm -f test hal_vmm
.PHONY: all run clean
