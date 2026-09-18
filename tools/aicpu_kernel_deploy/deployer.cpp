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

/* deployer — CP1 部署链工具(手动/诊断用): 经 CUSTAICPUKernel 迁移通道把 kernel .so 落到设备
 *
 * 溯源: fork 自 xcclpp aicpu-310p3/deployer.cpp(MIT), 按 P0-E0a 迁移;
 *       入口名改为参数传入(默认 HybmSdmaOrchBatch, 与产品 json 一致)。
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

#include "acl/acl.h"    // ACL 运行时(初始化/设备)
#include "acl/acl_rt.h" // aclrt* 二进制加载接口

int main(int argc, char **argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;                    // 目标设备号
    const char *json = argc >= 3 ? argv[2] : "libmf_sdma_orch.json"; // CUST 迁移 json 路径
    int secs = argc >= 4 ? atoi(argv[3]) : 600;                 // 保活时长(秒)
    // 入口函数名(默认产品编排 kernel 入口; 传错名时文件仍会落盘但 getfunc 返回非 0)
    const char *funcName = argc >= 5 ? argv[4] : "HybmSdmaOrchBatch";
    if (aclInit(nullptr) != ACL_SUCCESS) {                      // ACL 初始化
        printf("[E] aclInit\n");                                // 失败提示
        return 1;                                               // 失败退出
    }
    if (aclrtSetDevice(dev) != ACL_SUCCESS) {                   // 绑定目标设备
        printf("[E] SetDevice(%d)\n", dev);                     // 失败提示(设备号)
        return 1;                                               // 失败退出
    }
    aclrtBinaryLoadOptions lo = {};                             // 二进制加载选项容器
    aclrtBinaryLoadOption opt = {};                             // 单项选项
    opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;          // 选项类型 = CPU kernel 模式
    opt.value.cpuKernelMode = 1;                                // mode=1: H2D 迁移通道(CUSTAICPUKernel 专用)
    lo.numOpt = 1;                                              // 选项个数
    lo.options = &opt;                                          // 挂接选项数组
    aclrtBinHandle bh = nullptr;                                // 二进制句柄(出参)
    aclError r = aclrtBinaryLoadFromFile(json, &lo, &bh);       // 按 json 加载(mode=1 不落盘)
    printf("load ret=%d\n", (int)r);                            // 打印加载结果
    if (r != 0) {                                               // 加载失败
        return 1;                                               // 失败退出
    }
    // GetFunction 触发 CopySoAndNameToCurrentDevice → batchLoadsoFrombuf → 设备写文件
    aclrtFuncHandle fh = nullptr;                               // 函数句柄(出参)
    r = aclrtBinaryGetFunction(bh, funcName, &fh);              // 按入口名取函数(触发落盘)
    printf("getfunc ret=%d (0=文件已落盘)\n", (int)r);          // 打印取函数结果
    fflush(stdout);                                             // 刷出(后台运行时日志不丢)
    if (r != 0) {                                               // 取函数失败(json 入口名不匹配)
        return 1;                                               // 失败退出(文件已落盘但入口未验证)
    }
    printf("deployed, alive %ds...\n", secs);                   // 部署成功提示(保活倒计时)
    fflush(stdout);                                             // 刷出
    sleep(secs);                                                // 保活 → 文件不被清理链删除
    return 0;                                                   // 正常退出
}
