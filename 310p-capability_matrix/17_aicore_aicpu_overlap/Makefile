# 17_aicore_aicpu_overlap — E0a: MemFabric 构建的 AICore×AICPU 协同编排 kernel
# 在 CP1 内执行 + SQE 直投闭环验收（310p-aicore-aicpu-开发计划.md P0-E0a）
#
# 目标:
#   make kernel      编译 AICPU 编排 kernel .so + CUST 迁移 json + v1 版本对
#                    (hcc 交叉编译, 与 src/hybm/ops/hybm_kernel 同工具链)
#   make host_tools  编译 mini_kfc / deployer (g++, KFC 部署链工具)
#   make probe       编译 E0a 验收探针 (g++ + libruntime 内部符号)
#   make all         以上全部
#   make clean
#
# 纪律: CP1 按文件名 dlopen 缓存 —— 改 kernel 必 bump KVER 再部署
include ../common.mk

CANN_HOME ?= /usr/local/Ascend/cann-9.1.0
AICPU_CC := $(CANN_HOME)/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++
AICPU_DIR := aicpu_kernel
KVER ?= 1

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

all: host_tools kernel probe probe_e0b

# ---------- host 工具 (KFC 部署链, 源自 xcclpp aicpu-310p3 单一事实源的 fork) ----------
host_tools: $(HOST_TOOLS)

mini_kfc: mini_kfc.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_LDFLAGS)

deployer: deployer.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_LDFLAGS)

# ---------- E0a 验收探针 ----------
probe: probe_e0a.cc hybm_overlap_contract.h
	$(HOST_CXX) $(HOST_CXXFLAGS) -o probe_e0a $< $(HOST_LDFLAGS)

# ---------- E0b 池 GVA 探针(链接 memfabric 静态库, 07 示例同款) ----------
# 仓库根构建产物: cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
PROJ_ROOT := $(abspath ../..)
MF_LIBS := $(PROJ_ROOT)/build/src/smem/csrc/libmf_smem.a \
            $(PROJ_ROOT)/build/src/hybm/csrc/libmf_hybm_core.a \
            $(PROJ_ROOT)/build/src/acc_links/csrc/libacc_tcp_net.a
SMEM_INC := -I$(PROJ_ROOT)/src/smem/include/host

probe_e0b: probe_e0b.cc hybm_overlap_contract.h $(MF_LIBS)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SMEM_INC) -w -o $@ $< \
	    -Wl,--start-group $(MF_LIBS) -Wl,--end-group $(HOST_LDFLAGS) -pthread

# ---------- AICPU 编排 kernel (hcc 交叉编译, xcclpp 同款 flags) ----------
kernel: $(AICPU_SO) $(AICPU_JSON_CUST) $(VSO) $(VJSON)

$(AICPU_SO): $(AICPU_DIR)/hybm_overlap_kernel.cc hybm_overlap_contract.h
	$(AICPU_CC) -shared -O2 -std=c++17 -fPIC -o $@ $<

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
	rm -f $(HOST_TOOLS) probe_e0a probe_e0b kfc_min.json
	rm -f $(AICPU_SO) $(AICPU_JSON_CUST)
	rm -f $(AICPU_DIR)/libmf_overlap_v*.so $(AICPU_DIR)/libmf_overlap_v*.json
