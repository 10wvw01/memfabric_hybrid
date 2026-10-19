# tools/aicpu_kernel_deploy/Makefile — CP1 部署链工具构建(deployer/mini_kfc)
# 定位: 开发/诊断工具——不接入主构建(src/CMakeLists 未引用), 不进产品安装包
#       (顶层 install 规则不含本目录); mini_kfc 在设备 OS 执行任意 shell 命令,
#       禁止随产品运行时分发(见 README 安全告示)。常规使用无需本工具(库初始化
#       已内置自动部署), 仅预部署/部署链排查/库外 kernel 场景手动构建。
# 用法: make ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
# 产物: deployer(CUST 迁移落盘) + mini_kfc(KFC 通道设备内 shell)

ASCEND_HOME_PATH ?= /usr/local/Ascend/cann-9.1.0 # CANN 安装路径(可覆盖)
CANN_INC := $(ASCEND_HOME_PATH)/aarch64-linux/include # ACL 头文件目录
CANN_LIB := $(ASCEND_HOME_PATH)/aarch64-linux/lib64   # ACL 库目录
CXX ?= g++                                 # 主机编译器
CXXFLAGS := -O2 -std=c++17 -Wall -Werror   # 编译选项
LDFLAGS := -L$(CANN_LIB) -Wl,-rpath,$(CANN_LIB) -lascendcl -lruntime -ldl # 链接选项(rpath 免配 LD_LIBRARY_PATH)

TOOLS := deployer mini_kfc orch_deploy_helper                # 工具目标列表

all: $(TOOLS)                               # 默认构建全部工具

deployer: deployer.cpp                      # CUST 迁移落盘工具
	$(CXX) $(CXXFLAGS) -I$(CANN_INC) -o $@ $< $(LDFLAGS)

mini_kfc: mini_kfc.cpp                      # KFC 通道设备内 shell 工具
	$(CXX) $(CXXFLAGS) -I$(CANN_INC) -o $@ $< $(LDFLAGS)

orch_deploy_helper: orch_deploy_helper.cpp  # 固定命令 CP1 部署助手(库 fork+exec 调用)
	$(CXX) $(CXXFLAGS) -I$(CANN_INC) -o $@ $< $(LDFLAGS)

clean:                                      # 清理产物
	rm -f $(TOOLS)

.PHONY: all clean
