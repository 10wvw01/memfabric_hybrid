# 09_vmm_samedev：同设备跨进程 VMM


## 测试内容

两个进程使用同一设备，验证 VMM 共享句柄导入、映射、异步读写，并记录同步 D2D 对照。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

设备 2，两个 MPI 进程；32 MiB 主块、16 MiB 传输段。

**通过，退出码 0。**

异步读写及数据校验通过；同步共享 D2D 返回 207000。异步读 88.86 GB/s、写 89.17 GB/s。此前另一台驱动 26.1.1 机器的一次初始化超时没有进入数据阶段，未据此判定 API 不支持。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
DEV=2 bash 310p-capability_matrix/09_vmm_samedev/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

## 详细测试步骤、API 调用链与历史性能数据

以下保留原文的详细流程、结果和性能数据，环境及设备组合以各段原始记录为准。
本轮结果见上文；涉及不同环境或未复测拓扑时，分别阅读，不合并为一次测试结果。

### 09_vmm_samedev — VMM shareable handle 同设备跨进程

Atlas 300I Duo (310P3) / CANN 9.1.0 / MPI 双进程, 两 rank 同设备(dev0)。

## 结论

| 能力 | 结果 | 说明 |
|------|------|------|
| VMM 同设备跨进程 import/export | OK | 不需要 EnablePeerAccess |
| async D2D(同设备) | OK | 读写双向 pattern 校验通过 |
| sync D2D(同设备) | 失败 | ret=207000, 与跨设备/跨 die 一致, 必须用 async |
| host 直达(imported VA) | OK | H2D/D2H 各 16MB 校验通过 |
| memAttr 回退 | ACL_MEM_P2P_HUGE | gran=2MB, 32MB=16xgran |
| 带宽(同设备 async D2D) | ~88 GB/s | 设备内部拷贝, 非跨 PCIe |

## 测试项

1. **exporter**: MallocPhysical(七级回退)+Reserve+Map+SetAccess+H2D+Export+MPI发sh
2. **importer**: Import+Reserve+Map+SetAccess + async D2D 读写双向校验
3. **对照**: sync D2D → 预期失败(与跨设备一致)
5. **host 直达**: imported VA 上 D2H/H2D 各 16MB + 校验
4. **带宽**: async D2D 3预热+30x16MB+1Sync(仅正确性全过后执行)

判定: 1/2/5 全 OK 且 3 如期失败 → PASS。

## API 链

- **exporter**: `aclrtMallocPhysical` → `aclrtReserveMemAddress` → `aclrtMapMem` → `aclrtMemSetAccess` → H2D → `aclrtMemExportToShareableHandle(DISABLE_PID_VALIDATION)` → MPI 发 sh
- **importer**: `aclrtMemImportFromShareableHandle(sh, dev0)` → Reserve → Map → SetAccess → async D2D / host 直达

cleanup: importer 先释放 → MPI barrier → exporter 释放; DisablePeerAccess best-effort。

## 运行

```bash
./run.sh        # 默认 DEV=0
DEV=1 ./run.sh  # 换设备
```

## 预期输出

```
step 1 (r0): MallocPhysical(7attr)+Reserve+Map+H2D pat+Export sh1+send  OK
step 2 (r1): import+Reserve+Map+async D2D read+verify                    OK
step 2 (r1): async D2D reverse write + StreamSync                        OK
step 2 (r0): D2H verify reverse-written pattern                          OK
step 3 (r1): ctrl: sync D2D — expect FAIL                                EXPECTED-FAIL
step 5 (r1): host direct D2H/H2D read/write                              OK
step 5 (r0): D2H verify host-direct-written                              OK
step 4 (r1): bw D2D-read ~88 GB/s, D2D-write ~88 GB/s
step 2 (r0): final D2H verify reverse pattern (full 32MB)                OK
[09] RESULT: PASS
```
