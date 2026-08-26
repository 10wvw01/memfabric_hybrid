# 公共编译规则(各测试 Makefile include)
ASCEND_HOME ?= /usr/local/Ascend/cann-9.1.0
CANN_INCLUDE := $(ASCEND_HOME)/aarch64-linux/include
CANN_LIB := $(ASCEND_HOME)/aarch64-linux/lib64

CC := mpicc
CFLAGS := -I$(CANN_INCLUDE) -Wall -O2 -g -std=gnu99
LDLIBS := -L$(CANN_LIB) -lascendcl -Wl,-rpath,$(CANN_LIB) -ldl -lpthread -lmpi

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
