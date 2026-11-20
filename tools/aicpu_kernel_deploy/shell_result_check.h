#ifndef MF_TOOLS_SHELL_RESULT_CHECK_H
#define MF_TOOLS_SHELL_RESULT_CHECK_H

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "acl/acl.h"
#include "hybm_sdma_contract.h"

namespace mf_tools {
constexpr aclError SHELL_RESULT_UNCONFIRMED = 507018;
constexpr int EXIT_UNCONFIRMED = 2;
struct Stream {
    aclrtStream value = nullptr;
    ~Stream()
    {
        if (value)
            (void)aclrtDestroyStream(value);
    }
};
struct Buffer {
    void *value = nullptr;
    ~Buffer()
    {
        if (value)
            (void)aclrtFree(value);
    }
};

inline bool SafeSoName(const std::string &name)
{
    if (name.empty() || name.size() >= 128 || name.find("..") != std::string::npos)
        return false;
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    }
    return true;
}

inline aclError LoadFunction(const std::string &so, const char *function, aclrtFuncHandle &handle)
{
    char path[] = "/tmp/mf_tool_kfc_XXXXXX.json";
    int fd = mkstemps(path, 5);
    if (fd < 0) {
        printf("[E] create channel json\n");
        return 1;
    }
    const std::string json = "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"" + so +
                             "\",\"functionName\":\"" + function + "\"}}}";
    bool written = write(fd, json.data(), json.size()) == static_cast<ssize_t>(json.size());
    close(fd);
    if (!written) {
        unlink(path);
        printf("[E] write channel json\n");
        return 1;
    }
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions options{};
    options.numOpt = 1;
    options.options = &option;
    aclrtBinHandle binary = nullptr;
    aclError ret = aclrtBinaryLoadFromFile(path, &options, &binary);
    if (!ret)
        ret = aclrtBinaryGetFunction(binary, "K", &handle);
    unlink(path);
    if (ret)
        printf("[E] load checker/channel ret=%d so=%s function=%s\n", ret, so.c_str(), function);
    return ret;
}

inline aclError ExecuteShell(const char *command, bool &submitted)
{
    submitted = false;
    aclrtFuncHandle function = nullptr;
    auto ret = LoadFunction("kfc_min.so", "system", function);
    if (ret)
        return ret;
    Stream stream;
    ret = aclrtCreateStream(&stream.value);
    if (ret) {
        printf("[E] create command stream ret=%d\n", ret);
        return ret;
    }
    std::vector<char> bytes(command, command + strlen(command) + 1);
    aclrtLaunchKernelAttr timeout{};
    timeout.id = ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    timeout.value.timeout = 10;
    aclrtLaunchKernelCfg config{};
    config.attrs = &timeout;
    config.numAttrs = 1;
    ret = aclrtLaunchKernelWithHostArgs(function, 1, stream.value, &config, bytes.data(), bytes.size(), nullptr, 0);
    if (ret) {
        printf("cmd='%s' launch=%d\n", command, ret);
        return ret;
    }
    submitted = true;
    ret = aclrtSynchronizeStream(stream.value);
    printf("cmd='%s' launch=0 sync=%d\n", command, ret);
    return ret;
}

inline bool ReadCheck(aclrtFuncHandle function, const std::string &path, uint64_t nonce, const std::vector<char> &bytes,
                      ock::mf::HybmSdmaDeployResult &result, FILE *log = stdout)
{
    Buffer buffer;
    auto ret = aclrtMalloc(&buffer.value, bytes.size() + 128, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret) {
        fprintf(log, "[E] allocate checker buffer ret=%d\n", ret);
        return false;
    }
    if (!bytes.empty())
        ret = aclrtMemcpy(buffer.value, bytes.size(), bytes.data(), bytes.size(), ACL_MEMCPY_HOST_TO_DEVICE);
    ock::mf::HybmSdmaDeployCheck check{};
    check.expectedAddr = reinterpret_cast<uint64_t>(buffer.value);
    check.expectedSize = bytes.size();
    check.nonce = nonce;
    check.resultAddr = check.expectedAddr + ((bytes.size() + 63) & ~size_t(63));
    if (path.size() >= sizeof(check.path)) {
        fprintf(log, "[E] device path too long\n");
        return false;
    }
    memcpy(check.path, path.c_str(), path.size() + 1);
    if (!ret)
        ret = aclrtMemset(reinterpret_cast<void *>(check.resultAddr), sizeof(result), 0, sizeof(result));
    Stream stream;
    if (!ret)
        ret = aclrtCreateStream(&stream.value);
    if (!ret)
        ret = aclrtLaunchKernelWithHostArgs(function, 1, stream.value, nullptr, &check, sizeof(check), nullptr, 0);
    if (!ret)
        ret = aclrtSynchronizeStream(stream.value);
    if (!ret)
        ret = aclrtMemcpy(&result, sizeof(result), reinterpret_cast<void *>(check.resultAddr), sizeof(result),
                          ACL_MEMCPY_DEVICE_TO_HOST);
    fprintf(log, "check path=%s ret=%d status=%u nonce=%llu compared=%llu\n", path.c_str(), ret, result.status,
            static_cast<unsigned long long>(result.nonce), static_cast<unsigned long long>(result.compared));
    return !ret && result.nonce == nonce;
}
} // namespace mf_tools
#endif
