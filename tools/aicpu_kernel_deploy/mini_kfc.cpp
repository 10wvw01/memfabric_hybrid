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

/* mini_kfc — KFC 通道工具(诊断用): host 一条 launch, 设备 OS 内执行任意 shell 命令
 *
 * 溯源: fork 自 xcclpp aicpu-310p3/mini_kfc.cpp(MIT), 按 P0-E0a 迁移, 行为逐字节一致。
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

#include "acl/acl.h"    // ACL 运行时(初始化/设备)
#include "acl/acl_rt.h" // aclrt* 二进制加载/launch 接口

int main(int argc, char **argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;               // 目标设备号(命令在该设备 CP1 内执行)
    const char *cmd = argc >= 3 ? argv[2] : "echo hello";  // 待执行的 shell 命令(单条短命令)
    if (aclInit(nullptr) != ACL_SUCCESS) {                 // ACL 初始化
        printf("[E] aclInit\n");                           // 失败提示
        return 1;                                          // 失败退出
    }
    if (aclrtSetDevice(dev) != ACL_SUCCESS) {              // 绑定目标设备
        printf("[E] SetDevice(%d)\n", dev);                // 失败提示(设备号)
        return 1;                                          // 失败退出
    }
    // 最小 KFC json(functionName=system, kernelSo 指向不存在文件触发 dlsym 回退)
    FILE *f = fopen("kfc_min.json", "w");                  // 在 CWD 写临时 json
    if (f == nullptr) {                                    // 写文件失败
        printf("[E] write kfc_min.json\n");                // 失败提示(权限/CWD)
        return 1;                                          // 失败退出
    }
    fprintf(f, "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"kfc_min.so\","
               "\"functionName\":\"system\"}}}");          // KFC 路由声明(op 名 K)
    fclose(f);                                             // 落盘关闭
    aclrtBinaryLoadOptions lo = {};                        // 二进制加载选项容器
    aclrtBinaryLoadOption opt = {};                        // 单项选项
    opt.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;     // 选项类型 = CPU kernel 模式
    opt.value.cpuKernelMode = 0;                           // mode=0: 不迁移, 直接按 json 路由
    lo.numOpt = 1;                                         // 选项个数
    lo.options = &opt;                                     // 挂接选项数组
    aclrtBinHandle bh = nullptr;                           // 二进制句柄(出参)
    if (aclrtBinaryLoadFromFile("kfc_min.json", &lo, &bh) != 0) { // 按 json 加载
        printf("[E] load\n");                              // 失败提示
        return 1;                                          // 失败退出
    }
    aclrtFuncHandle fh = nullptr;                          // 函数句柄(出参)
    if (aclrtBinaryGetFunction(bh, "K", &fh) != 0) {       // 取 op "K" 的函数句柄
        printf("[E] func\n");                              // 失败提示
        return 1;                                          // 失败退出
    }
    aclrtStream s = nullptr;                               // 承载本次 launch 的流
    aclrtCreateStream(&s);                                 // 创建流
    char buf[1024];                                        // 命令串缓冲(launch 参数)
    strncpy(buf, cmd, 1023);                               // 拷贝命令(留 1 字节终止)
    buf[sizeof(buf) - 1] = '\0';                           // strncpy 不保证终止, strlen 前必须显式补
    // 控制字符校验: 含控制字符的命令一定是误传(防误用)
    for (const char *p = buf; *p; p++) {                   // 逐字节扫描命令串
        if ((unsigned char)*p < 0x20 && *p != '\t') {      // 非法控制字符(放行 tab)
            printf("[E] cmd 含控制字符 (0x%02x)\n", (unsigned char)*p); // 失败提示(字符值)
            return 1;                                      // 失败退出
        }
    }
    // 命令串作为 host args 透传 → CP1 内 system() 执行; 退出码经 stream sync 传回
    aclError r = aclrtLaunchKernelWithHostArgs(fh, 1, s, nullptr, buf, strlen(buf) + 1, nullptr, 0);
    aclError sr = aclrtSynchronizeStream(s);               // 同步取回命令退出码
    printf("cmd='%s' launch=%d sync=%d\n", cmd, (int)r, (int)sr); // 打印结果(解析 sync= 判定)
    return 0;                                              // 恒 0(结果看 stdout 的 sync=)
}
