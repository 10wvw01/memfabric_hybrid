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

/* mini_kfc — KFC 通道工具(诊断用): host 一条 launch, 设备 OS 内执行任意 shell 命令
 *
 * 机制: json 声明 KFCKernel + functionName "system" + kernelSo 指向不存在的 .so
 *       → CP1 内 KFC handler dlopen 失败不报错、回退 dlsym(RTLD_DEFAULT) 命中 libc
 *       的 system() → param(host args 透传的命令串)成为其参数。零部署零上传。
 * 输出语义(重要, X9 实测勘正): "sync=N" = 命令退出码(0=成功, 非零→507018 形式回传);
 *       本工具 main 恒 return 0, 调用方必须解析 stdout 里的 sync= 判定结果。
 * 安全告示: 本工具在设备 OS 内执行任意 shell 命令, 仅限开发/诊断环境使用,
 *       禁止随产品运行时分发(产品库内置部署仅执行固定命令串, 见 hybm_sdma_orchestrator.cpp)。
 * 用法: ./mini_kfc <dev> "<命令>"
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "acl/acl.h"
#include "acl/acl_rt.h"

int main(int argc, char **argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;
    const char *cmd = argc >= 3 ? argv[2] : "echo hello";
    if (aclInit(nullptr) != ACL_SUCCESS) {
        printf("[E] aclInit\n");
        return 1;
    }
    if (aclrtSetDevice(dev) != ACL_SUCCESS) {
        printf("[E] SetDevice(%d)\n", dev);
        return 1;
    }
    // 最小 KFC json(functionName=system, kernelSo 指向不存在文件触发 dlsym 回退)
    FILE *f = fopen("kfc_min.json", "w");
    if (f == nullptr) {
        printf("[E] write kfc_min.json\n");
        return 1;
    }
    fprintf(f, "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"kfc_min.so\","
               "\"functionName\":\"system\"}}}");
    fclose(f);
    aclrtBinaryLoadOptions lo = {};
    aclrtBinaryLoadOption opt = {};
    opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    opt.value.cpuKernelMode = 0; // mode=0: 不迁移, 直接按 json 路由
    lo.numOpt = 1;
    lo.options = &opt;
    aclrtBinHandle bh = nullptr;
    if (aclrtBinaryLoadFromFile("kfc_min.json", &lo, &bh) != 0) {
        printf("[E] load\n");
        return 1;
    }
    aclrtFuncHandle fh = nullptr;
    if (aclrtBinaryGetFunction(bh, "K", &fh) != 0) {
        printf("[E] func\n");
        return 1;
    }
    aclrtStream s = nullptr;
    aclrtCreateStream(&s);
    char buf[1024];
    strncpy(buf, cmd, 1023);
    buf[sizeof(buf) - 1] = '\0'; // strncpy 不保证终止, strlen 前必须显式补
    // 含控制字符的命令一定是误传(放行 tab)
    for (const char *p = buf; *p; p++) {
        if ((unsigned char)*p < 0x20 && *p != '\t') {
            printf("[E] cmd 含控制字符 (0x%02x)\n", (unsigned char)*p);
            return 1;
        }
    }
    // 命令串作为 host args 透传 → CP1 内 system() 执行; 退出码经 stream sync 传回
    aclError r = aclrtLaunchKernelWithHostArgs(fh, 1, s, nullptr, buf, strlen(buf) + 1, nullptr, 0);
    aclError sr = aclrtSynchronizeStream(s);
    printf("cmd='%s' launch=%d sync=%d\n", cmd, (int)r, (int)sr);
    return 0; // 恒 0, 结果看 stdout 的 sync=
}
