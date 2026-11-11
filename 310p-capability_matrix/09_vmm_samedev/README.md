# 09_vmm_samedev — VMM shareable handle 同设备跨进程

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
