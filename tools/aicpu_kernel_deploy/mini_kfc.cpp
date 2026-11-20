/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/*
 * This file derives from the xcclpp project (aicpu-310p3/mini_kfc.cpp), which is
 * licensed under the MIT License. The original copyright and permission notice:
 *
 * Copyright (c) xcclpp authors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// 开发诊断工具：507018保持为结果未确认，文件操作可使用与产品库相同的AICPU校验入口。
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "shell_result_check.h"

namespace {
struct Options {
    std::string host, target, verifier;
    uint64_t nonce = 0;
    bool present = false, absent = false;
};
void Usage()
{
    puts("mini_kfc <device> <command> [--expect-so <host.so> <device-path> <nonce> | --expect-absent <device-path> "
         "<nonce>] [--verifier <device-so-name>]");
    puts("Exit: 0=confirmed, 1=error/check failed, 2=shell result unconfirmed. Checker SO must export "
         "HybmSdmaVerifyDeploy.");
}
bool Nonce(const char *text, uint64_t &value)
{
    char *end = nullptr;
    errno = 0;
    if (!*text || *text == '-')
        return false;
    auto parsed = strtoull(text, &end, 10);
    if (errno || *end || !parsed)
        return false;
    value = parsed;
    return true;
}
bool Parse(int argc, char **argv, Options &o)
{
    for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--verifier" && i + 1 < argc)
            o.verifier = argv[++i];
        else if (flag == "--expect-so" && i + 3 < argc && !o.present && !o.absent) {
            o.present = true;
            o.host = argv[++i];
            o.target = argv[++i];
            if (!Nonce(argv[++i], o.nonce))
                return false;
        } else if (flag == "--expect-absent" && i + 2 < argc && !o.present && !o.absent) {
            o.absent = true;
            o.target = argv[++i];
            if (!Nonce(argv[++i], o.nonce))
                return false;
        } else
            return false;
    }
    if (!o.present && !o.absent)
        return o.verifier.empty();
    if (o.target.empty())
        return false;
    const auto basename = o.target.substr(o.target.find_last_of('/') + 1);
    if (o.verifier.empty())
        o.verifier = basename;
    if (!mf_tools::SafeSoName(o.verifier))
        return false;
    if (o.target.find('/') == std::string::npos)
        o.target = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/" + o.target;
    return o.target.size() < sizeof(ock::mf::HybmSdmaDeployCheck::path);
}
int Run(const char *command, const Options &o, const std::vector<char> &bytes)
{
    aclrtFuncHandle checker = nullptr;
    ock::mf::HybmSdmaDeployResult result{};
    // 删除前先加载校验代码，文件删除后继续使用本进程中已加载的入口。
    if (o.absent) {
        if (mf_tools::LoadFunction(o.verifier, "HybmSdmaVerifyDeploy", checker) ||
            !mf_tools::ReadCheck(checker, o.target, o.nonce, {}, result) || (result.status != 0 && result.status != 6))
            return 1;
    }
    bool submitted = false;
    auto ret = mf_tools::ExecuteShell(command, submitted);
    if (!submitted)
        return 1;
    if (ret && ret != mf_tools::SHELL_RESULT_UNCONFIRMED)
        return 1;
    if (ret == mf_tools::SHELL_RESULT_UNCONFIRMED)
        puts("shell_status=UNCONFIRMED");
    if (!o.present && !o.absent)
        return ret ? mf_tools::EXIT_UNCONFIRMED : 0;
    if (!checker && mf_tools::LoadFunction(o.verifier, "HybmSdmaVerifyDeploy", checker))
        return 1;
    if (!mf_tools::ReadCheck(checker, o.target, o.nonce, bytes, result) || result.status ||
        result.compared != bytes.size()) {
        puts("verification=FAILED");
        return 1;
    }
    puts(o.absent ? "verification=ABSENCE_CONFIRMED" : "verification=BYTE_FOR_BYTE_CONFIRMED");
    return 0;
}
} // namespace
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        Usage();
        return 0;
    }
    if (argc < 3) {
        Usage();
        return 1;
    }
    char *end = nullptr;
    long dev = strtol(argv[1], &end, 10);
    Options o;
    if (!*argv[1] || *end || dev < 0 || dev > INT_MAX || !Parse(argc, argv, o) || strlen(argv[2]) >= 512) {
        puts("[E] invalid arguments");
        return 1;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(argv[2]); *p; ++p)
        if (*p < 0x20 && *p != '\t') {
            puts("[E] command contains control characters");
            return 1;
        }
    std::vector<char> bytes;
    if (o.present) {
        std::ifstream input(o.host, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad() || bytes.empty()) {
            puts("[E] cannot read nonempty source SO");
            return 1;
        }
    }
    if (aclInit(nullptr)) {
        puts("[E] aclInit");
        return 1;
    }
    auto ret = aclrtSetDevice(static_cast<int>(dev));
    if (ret) {
        printf("[E] SetDevice(%ld) ret=%d\n", dev, ret);
        (void)aclFinalize();
        return 1;
    }
    int status = Run(argv[2], o, bytes);
    (void)aclrtResetDevice(static_cast<int>(dev));
    (void)aclFinalize();
    return status;
}
