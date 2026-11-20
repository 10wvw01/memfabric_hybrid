#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include "shell_result_check.h"

static void Check(const char *api, aclError ret)
{
    if (ret != ACL_SUCCESS) {
        std::cerr << "ERROR " << api << " ret=" << ret << std::endl;
        std::exit(1);
    }
}

static bool Inspect(const std::string &command, const std::vector<char> &bytes,
                    std::map<std::string, aclrtFuncHandle> &loaded)
{
    std::istringstream input(command);
    std::string mode, path;
    uint64_t nonce = 0;
    if (!(input >> mode >> path >> nonce) || !nonce || (mode != "present" && mode != "absent")) {
        std::cerr << "ERROR invalid inspection request: " << command << std::endl;
        return false;
    }
    auto &function = loaded[path];
    if (!function) {
        const auto name = path.substr(path.find_last_of('/') + 1);
        if (mode == "absent" || !mf_tools::SafeSoName(name)) {
            std::cerr << "ERROR verifier unavailable: " << path << std::endl;
            return false;
        }
        if (mf_tools::LoadFunction(name, "HybmSdmaVerifyDeploy", function))
            return false;
    }
    // 保留已加载入口，待目标文件删除后使用同一入口检查 SO 与 owner 均已不存在。
    ock::mf::HybmSdmaDeployResult result{};
    const std::vector<char> empty;
    const auto &expected = mode == "present" ? bytes : empty;
    return mf_tools::ReadCheck(function, path, nonce, expected, result, stderr) && result.status == 0 &&
           result.compared == expected.size();
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "ERROR usage: inspect device host_so" << std::endl;
        return 1;
    }
    std::ifstream source(argv[2], std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    if (source.bad() || bytes.empty()) {
        std::cerr << "ERROR cannot read source: " << argv[2] << std::endl;
        return 1;
    }
    const int device = std::atoi(argv[1]);
    Check("aclInit", aclInit(nullptr));
    Check("set device", aclrtSetDevice(device));
    std::map<std::string, aclrtFuncHandle> loaded;
    std::cout << "READY" << std::endl;
    std::string command;
    while (std::getline(std::cin, command) && command != "exit")
        std::cout << (Inspect(command, bytes, loaded) ? 0 : 1) << std::endl;
    Check("reset device", aclrtResetDevice(device));
    Check("aclFinalize", aclFinalize());
}
