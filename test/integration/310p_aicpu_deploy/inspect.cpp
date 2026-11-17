#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include "acl/acl.h"
#include "acl/acl_rt.h"

static void Check(const char *api, aclError ret)
{
    if (ret != ACL_SUCCESS) {
        std::cerr << "ERROR " << api << " ret=" << ret << std::endl;
        std::exit(1);
    }
}

static aclrtFuncHandle Load(const char *jsonPath)
{
    std::ofstream json(jsonPath);
    json << R"({"K":{"opInfo":{"opKernelLib":"KFCKernel","kernelSo":"kfc_min.so","functionName":"system"}}})";
    json.close();
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions options{};
    options.options = &option;
    options.numOpt = 1;
    aclrtBinHandle binary = nullptr;
    Check("load inspector", aclrtBinaryLoadFromFile(jsonPath, &options, &binary));
    aclrtFuncHandle function = nullptr;
    Check("get inspector", aclrtBinaryGetFunction(binary, "K", &function));
    return function;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "ERROR usage: inspect device json_path" << std::endl;
        return 1;
    }
    const int device = std::atoi(argv[1]);
    Check("aclInit", aclInit(nullptr));
    Check("set device", aclrtSetDevice(device));
    const auto function = Load(argv[2]);
    aclrtStream stream = nullptr;
    Check("create stream", aclrtCreateStream(&stream));
    aclrtLaunchKernelAttr attr{};
    attr.id = ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = 25;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    std::cout << "READY" << std::endl;
    std::string command;
    while (std::getline(std::cin, command) && command != "exit") {
        auto ret =
            aclrtLaunchKernelWithHostArgs(function, 1, stream, &cfg, command.data(), command.size() + 1, nullptr, 0);
        if (ret == ACL_SUCCESS)
            ret = aclrtSynchronizeStreamWithTimeout(stream, 30000);
        std::cerr << command << " ret=" << ret << std::endl;
        std::cout << ret << std::endl;
    }
    Check("destroy stream", aclrtDestroyStream(stream));
    Check("reset device", aclrtResetDevice(device));
    Check("aclFinalize", aclFinalize());
}
