/* deployer — CP1 部署链工具：经 CUSTAICPUKernel 迁移通道把 kernel .so 落到设备
 * 溯源: fork 自 xcclpp aicpu-310p3/deployer.cpp (MIT), 按 P0-E0a 迁移;
 *       functionName 改 HybmOverlapRun, 默认 json 改 libmf_overlap_v1。
 * 机制: aclrtBinaryLoadFromFile(mode=1) 本身不落盘 —— aclrtBinaryGetFunction 才触发
 *       CopySoAndNameToCurrentDevice → batchLoadsoFrombuf → 设备写
 *       /home/CustAiCpuUser/lib/<so名>.<devId>.<vfId>.<hostPid>;
 *       随后 sleep 保活 (进程退出会触发 deleteCustOp 清理链删文件)。
 * 用法: ./deployer <dev> <CUST_json路径> <保活秒数>
 */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "acl/acl.h"
#include "acl/acl_rt.h"

int main(int argc, char** argv)
{
    int dev = argc >= 2 ? atoi(argv[1]) : 0;                                       // 目标设备号
    const char* json = argc >= 3 ? argv[2] : "aicpu_kernel/libmf_overlap_aicpu.json"; // CUST 迁移 json
    int secs = argc >= 4 ? atoi(argv[3]) : 600;                                    // 保活时长(秒)
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
    opt.value.cpuKernelMode = 1; // mode=1: H2D 迁移通道(CUSTAICPUKernel 专用)
    lo.numOpt = 1;
    lo.options = &opt;
    aclrtBinHandle bh = nullptr;
    aclError r = aclrtBinaryLoadFromFile(json, &lo, &bh);
    printf("load ret=%d\n", (int)r);
    if (r != 0) {
        return 1;
    }
    // ★ GetFunction 触发 CopySoAndNameToCurrentDevice → batchLoadsoFrombuf → 设备写文件
    aclrtFuncHandle fh = nullptr;
    r = aclrtBinaryGetFunction(bh, "HybmOverlapRun", &fh);
    printf("getfunc ret=%d (0=文件已落盘)\n", (int)r);
    fflush(stdout);
    if (r != 0) {
        return 1;
    }
    printf("deployed, alive %ds...\n", secs);
    fflush(stdout);
    sleep(secs); // 保活 → 文件不被 deleteCustOp 清理
    return 0;
}
