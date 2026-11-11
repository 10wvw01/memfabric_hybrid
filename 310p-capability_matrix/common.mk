# 公共编译规则(各测试 Makefile include)
ASCEND_HOME ?= $(if $(ASCEND_HOME_PATH),$(ASCEND_HOME_PATH),/usr/local/Ascend/cann)
CANN_INCLUDE := $(ASCEND_HOME)/aarch64-linux/include
CANN_LIB := $(ASCEND_HOME)/aarch64-linux/lib64

MATRIX_N ?= 8
CC := mpicc
CFLAGS := -I$(CANN_INCLUDE) -Wall -O2 -g -std=gnu99 -DMATRIX_N=$(MATRIX_N)
LDLIBS := -L$(CANN_LIB) -lascendcl -Wl,-rpath,$(CANN_LIB) -ldl -lpthread -lmpi

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
