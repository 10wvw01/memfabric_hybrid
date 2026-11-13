# 09：同一设备的跨进程VMM共享

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/09_vmm_samedev
DEV=2 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 设备与大小 | 两个rank均使用设备2；主要物理块32MiB，Host/带宽16MiB |
| 导入、映射、异步D2D双向校验 | 通过 |
| Host直接H2D/D2H访问导入映射 | 校验通过 |
| 同步D2D读取导入映射 | 返回207000 |
| 带宽 | 16MiB，3次预热、30次异步提交后同步：读88.79、写88.49 GB/s |
| 总结果 | PASS，两个rank退出0 |

同一设备跨进程的VMM映射支持异步D2D双向读写；同步D2D读取该映射返回207000。

## 原有实验过程与数据

以下保留CANN 9.1.0下的原有实验条件、步骤、输出及分析。2026-11-13的执行情况见上表。

Atlas 300I Duo (310P3) / CANN 9.1.0 / MPI 双进程, 两 rank 同设备(dev0)。

## 结论

| 能力 | 结果 | 说明 |
|------|------|------|
| VMM 同设备跨进程 import/export | OK | 不需要 EnablePeerAccess |
| async D2D(同设备) | OK | 读写双向 pattern 校验通过 |
| sync D2D(同设备) | 失败 | 导入映射同步D2D返回207000，异步双向校验通过 |
| host 直达(imported VA) | OK | H2D/D2H 各 16MB 校验通过 |
| memAttr 回退 | ACL_MEM_P2P_HUGE | gran=2MB, 32MB=16xgran |
| 带宽(同设备 async D2D) | ~88 GB/s | 设备内部拷贝, 非跨 PCIe |

## 测试项

1. **exporter**: MallocPhysical(七级回退)+Reserve+Map+SetAccess+H2D+Export+MPI发sh
2. **importer**: Import+Reserve+Map+SetAccess + async D2D 读写双向校验
3. **对照**: sync D2D → 记录共享映射同步D2D返回值
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
