# tools/aicpu_kernel_deploy/Makefile — CP1 部署链工具构建(deployer/mini_kfc)
# 用法: make ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
# 产物: deployer(CUST 迁移落盘) + mini_kfc(KFC 通道设备内 shell)

ASCEND_HOME_PATH ?= /usr/local/Ascend/cann-9.1.0 # CANN 安装路径(可覆盖)
CANN_INC := $(ASCEND_HOME_PATH)/aarch64-linux/include # ACL 头文件目录
CANN_LIB := $(ASCEND_HOME_PATH)/aarch64-linux/lib64   # ACL 库目录
CXX ?= g++                                 # 主机编译器
CXXFLAGS := -O2 -std=c++17 -Wall -Werror   # 编译选项
LDFLAGS := -L$(CANN_LIB) -Wl,-rpath,$(CANN_LIB) -lascendcl -lruntime -ldl # 链接选项(rpath 免配 LD_LIBRARY_PATH)

TOOLS := deployer mini_kfc                  # 工具目标列表

all: $(TOOLS)                               # 默认构建全部工具

deployer: deployer.cpp                      # CUST 迁移落盘工具
	$(CXX) $(CXXFLAGS) -I$(CANN_INC) -o $@ $< $(LDFLAGS)

mini_kfc: mini_kfc.cpp                      # KFC 通道设备内 shell 工具
	$(CXX) $(CXXFLAGS) -I$(CANN_INC) -o $@ $< $(LDFLAGS)

clean:                                      # 清理产物
	rm -f $(TOOLS)

.PHONY: all clean
