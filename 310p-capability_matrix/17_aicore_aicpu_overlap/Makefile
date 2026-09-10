# 17_aicore_aicpu_overlap — E0a/E0b/E0c: MemFabric AICore×AICPU 协同验收三连
# (310p-aicore-aicpu-开发计划.md P0; P1-WP1 起 kernel 源在 src/hybm/ops/hybm_kernel/)
#
# 目标:
#   make kernel      编译 AICPU 编排 kernel .so + CUST 迁移 json + v$(KVER) 版本对
#                    (hcc, 与主构建 mf_overlap_kernel 目标字节等价)
#   make host_tools  编译 mini_kfc / deployer (g++, KFC 部署链工具)
#   make probe       E0a 验收探针
#   make probe_e0b   E0b 池 GVA 探针(链接 memfabric 静态库)
#   make probe_e0c   E0c 池上协同探针(bisheng host+AICore 统一编译)
#   make all         以上全部
#
# 纪律: CP1 按文件名 dlopen 缓存 —— 改 kernel 必 bump KVER 再部署
include ../common.mk

CANN_HOME ?= /usr/local/Ascend/cann-9.1.0
AICPU_CC := $(CANN_HOME)/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++
AICPU_DIR := aicpu_kernel
KVER ?= 1

# ---- 仓库根与单一事实源(P1-WP1 起 kernel/contract 在主构建树) ----
PROJ_ROOT := $(abspath ../..)
KERNEL_SRC_DIR := $(PROJ_ROOT)/src/hybm/ops/hybm_kernel
KERNEL_SRC := $(KERNEL_SRC_DIR)/hybm_overlap_kernel.cc
KERNEL_CONTRACT := $(KERNEL_SRC_DIR)/hybm_overlap_contract.h
KERNEL_INC := -I$(KERNEL_SRC_DIR)

# 仓库根构建产物: cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
MF_LIBS := $(PROJ_ROOT)/build/src/smem/csrc/libmf_smem.a \
           $(PROJ_ROOT)/build/src/hybm/csrc/libmf_hybm_core.a \
           $(PROJ_ROOT)/build/src/acc_links/csrc/libacc_tcp_net.a
SMEM_INC := -I$(PROJ_ROOT)/src/smem/include/host

# kernel 产物: 迁移源 so(CUST 通道用) + 版本对 so(launch 用, KVER 纪律)
AICPU_SO := $(AICPU_DIR)/libmf_overlap_aicpu.so
AICPU_JSON_CUST := $(AICPU_DIR)/libmf_overlap_aicpu.json
VSO := $(AICPU_DIR)/libmf_overlap_v$(KVER).so
VJSON := $(AICPU_DIR)/libmf_overlap_v$(KVER).json

# host 工具/探针链接: ascendcl + runtime(rtStreamGetSqid 内部符号) + dl
HOST_CXX := g++
HOST_CXXFLAGS := -I$(CANN_INCLUDE) -I. -O2 -Wall -std=c++17
HOST_LDFLAGS := -L$(CANN_LIB) -Wl,-rpath,$(CANN_LIB) -lascendcl -lruntime -ldl

HOST_TOOLS := mini_kfc deployer

.PHONY: all kernel host_tools probe clean

all: host_tools kernel probe probe_e0b probe_e0c probe_p1_shm

# ---------- host 工具 (KFC 部署链, 源自 xcclpp aicpu-310p3 单一事实源的 fork) ----------
host_tools: $(HOST_TOOLS)

mini_kfc: mini_kfc.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_LDFLAGS)

deployer: deployer.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_LDFLAGS)

# ---------- E0a 验收探针 ----------
probe: probe_e0a.cc $(KERNEL_CONTRACT)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(KERNEL_INC) -o probe_e0a $< $(HOST_LDFLAGS)

# ---------- E0b 池 GVA 探针 ----------
probe_e0b: probe_e0b.cc $(KERNEL_CONTRACT) $(MF_LIBS)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(KERNEL_INC) $(SMEM_INC) -w -o $@ $< \
	    -Wl,--start-group $(MF_LIBS) -Wl,--end-group $(HOST_LDFLAGS) -pthread

# ---------- E0c 池上协同探针(bisheng 统一编译 host+AICore kernel, dav-2002) ----------
BISHENG := $(CANN_HOME)/bin/bisheng
ASC_INC := -I$(CANN_HOME)/aarch64-linux/include -I$(CANN_HOME)/aarch64-linux/ascendc/include

probe_e0c: probe_e0c.asc $(KERNEL_CONTRACT) $(MF_LIBS)
	$(BISHENG) --npu-arch=dav-2002 -O2 -std=c++17 -w -x asc $< -x none -o $@ \
	    $(ASC_INC) $(KERNEL_INC) $(SMEM_INC) -I. \
	    -Wl,--start-group $(MF_LIBS) -Wl,--end-group $(HOST_LDFLAGS) -pthread -lm

# ---------- P1/WP3 验证探针(smem_shm API 走产品化挂载路径) ----------
HYBM_INTERNAL_INC := -I$(PROJ_ROOT)/src/hybm/csrc/common -I$(PROJ_ROOT)/src/util/csrc

probe_p1_shm: probe_p1_shm.cc $(MF_LIBS)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SMEM_INC) $(HYBM_INTERNAL_INC) -w -o $@ $< \
	    -Wl,--start-group $(MF_LIBS) -Wl,--end-group $(HOST_LDFLAGS) -pthread

# ---------- AICPU 编排 kernel (hcc, 与主构建 mf_overlap_kernel 目标字节等价) ----------
kernel: $(AICPU_SO) $(AICPU_JSON_CUST) $(VSO) $(VJSON)

$(AICPU_SO): $(KERNEL_SRC) $(KERNEL_CONTRACT)
	$(AICPU_CC) -shared -O2 -std=c++17 -fPIC -o $@ $(KERNEL_SRC)

# CUST 迁移 json: mode=1 专用(opKernelLib=CUSTAICPUKernel, deployer 喂这个)
$(AICPU_JSON_CUST):
	printf '{\n    "HybmOverlapRun": {\n        "opInfo": {\n            "opKernelLib": "CUSTAICPUKernel",\n            "kernelSo": "libmf_overlap_aicpu.so",\n            "functionName": "HybmOverlapRun"\n        }\n    }\n}\n' > $@

# 版本对: 复制 so + 生成 launch json(opKernelLib=AICPUKernel, cpuMode=0 喂这个)
$(VSO): $(AICPU_SO)
	cp $(AICPU_SO) $(VSO)
	printf '{\n    "HybmOverlapRun": {\n        "opInfo": {\n            "opKernelLib": "AICPUKernel",\n            "kernelSo": "libmf_overlap_v$(KVER).so",\n            "functionName": "HybmOverlapRun"\n        }\n    }\n}\n' > $(VJSON)

$(VJSON): $(VSO)
	@test -f $@ || printf '{\n    "HybmOverlapRun": {\n        "opInfo": {\n            "opKernelLib": "AICPUKernel",\n            "kernelSo": "libmf_overlap_v$(KVER).so",\n            "functionName": "HybmOverlapRun"\n        }\n    }\n}\n' > $@

clean:
	rm -f $(HOST_TOOLS) probe_e0a probe_e0b probe_e0c probe_p1_shm kfc_min.json
	rm -f $(AICPU_SO) $(AICPU_JSON_CUST)
	rm -f $(AICPU_DIR)/libmf_overlap_v*.so $(AICPU_DIR)/libmf_overlap_v*.json
