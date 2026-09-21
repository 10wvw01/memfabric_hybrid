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
 * This file derives from the xcclpp project (aicpu-310p3/deployer.cpp), which is
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

/* deployer — CP1 部署链工具(手动/诊断用): 经 CUSTAICPUKernel 迁移通道把 kernel .so 落到设备
 *
 * 入口名经参数传入(默认 HybmSdmaOrchEpoch, 与产品 json functionName 一致)。
 * 机制: aclrtBinaryLoadFromFile(cpuMode=1) 本身不落盘 —— aclrtBinaryGetFunction 才触发
 *       CopySoAndNameToCurrentDevice → batchLoadsoFrombuf → 设备写
 *       /home/CustAiCpuUser/lib/<so名>.<devId>.<vfId>.<hostPid>;
 *       随后 sleep 保活(迁移进程退出会触发清理链延迟删除设备侧文件)。
 * 说明: 产品库初始化已内置自动部署(见 hybm_sdma_orchestrator.cpp); 本工具用于
 *       预部署、部署链排查与库外 kernel 的手动落盘。
 * 用法: ./deployer <dev> <CUST_json路径> <保活秒数> [入口函数名]
 */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "acl/acl.h"
#include "acl/acl_rt.h"

int main(int argc, char **argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;
    const char *json = argc >= 3 ? argv[2] : "libmf_sdma_orch.json";
    int secs = argc >= 4 ? atoi(argv[3]) : 600;
    const char *funcName = argc >= 5 ? argv[4] : "HybmSdmaOrchEpoch"; // 传错名时文件仍落盘但 getfunc 非 0
    if (aclInit(nullptr) != ACL_SUCCESS) {
        printf("[E] aclInit\n");
        return 1;
    }
    if (aclrtSetDevice(dev) != ACL_SUCCESS) {
        printf("[E] SetDevice(%d)\n", dev);
        return 1;
    }
    aclrtBinaryLoadOptions lo = {};
    aclrtBinaryLoadOption opt = {};
    opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    opt.value.cpuKernelMode = 1; // mode=1: H2D 迁移通道, 本身不落盘
    lo.numOpt = 1;
    lo.options = &opt;
    aclrtBinHandle bh = nullptr;
    aclError r = aclrtBinaryLoadFromFile(json, &lo, &bh);
    printf("load ret=%d\n", (int)r);
    if (r != 0) {
        return 1;
    }
    aclrtFuncHandle fh = nullptr;
    r = aclrtBinaryGetFunction(bh, funcName, &fh); // GetFunction 才触发设备写文件
    printf("getfunc ret=%d (0=文件已落盘)\n", (int)r);
    fflush(stdout); // 后台运行时日志不丢
    if (r != 0) {
        return 1;
    }
    printf("deployed, alive %ds...\n", secs);
    fflush(stdout);
    sleep(secs); // 保活 → 文件不被清理链删除
    return 0;
}
