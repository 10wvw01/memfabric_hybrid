# 03_ipc_cross — IPC key 链路跨卡(PCIe)

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 2)。

## 验证能力
- IPC 跨卡 PEER_ACCESS import ✓
- 跨卡 async D2D 读+写 ✓(PEER imported VA,双向数据校验)
- host 直达远端 H2D/D2H ✓(imported VA 上直接同步 memcpy)

## API 链
- export:`aclrtMalloc(HUGE_FIRST_P2P)` → H2D pattern → `aclrtIpcMemGetExportKey(DISABLE_PID_VALIDATION)` → MPI 发 key
- import:`aclrtIpcMemImportByKey(ENABLE_PEER_ACCESS)`(内部双向 EnablePeerAccess)
- 拷贝:`aclrtMemcpyAsync(D2D)` + `aclrtSynchronizeStream`;host 直达:imported VA 上 `aclrtMemcpy`
- cleanup:双方 `aclrtIpcMemClose`;exporter `aclrtFree`;ResetDevice 前 `aclrtDeviceDisablePeerAccess` 拆 P2P

校验 pattern:`value=(rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 每步不同防残留假阳性。
带宽:正确性全过后,3 预热 + 30×16MB `MemcpyAsync` 提交 + 末尾 1 次 `StreamSync`(禁同步循环)。

## 运行
```
./run.sh                  # 默认 dev0→dev2(跨卡)
A_DEV=1 B_DEV=4 ./run.sh  # 原 ✗ 单元格(10 实测 CANN 9.1.0 下 VMM 已可达)
A_DEV=0 B_DEV=0 ./run.sh  # 同设备参考(PEER flag 非为同设备设计)
```

## 结论

跨卡 IPC-PEER 全功能可用;RESULT: PASS。

| 拓扑 | RESULT | bw 读 (p→local) | bw 写 (local→p) |
|---|---|---|---|
| 0→2 跨卡(默认) | PASS | ~11 GB/s | ~14 GB/s |
| 1→4 ✗单元格 | PASS | 11.25 GB/s | 13.14 GB/s |
| 4→1 ✗单元格反向 | PASS | **1.29 GB/s** | 13.00 GB/s |
| 0→0 同设备参考 | FAIL | — | — |

- **跨卡 IPC-PEER 全功能通过**:import / async D2D 双向 / host 直达 / 逐字校验全 OK。
  原 ✗ 单元格(dev1↔dev4)在 CANN 9.1.0 下 VMM 已可达(见 10),IPC-PEER 一直可用。
- **带宽**:跨卡 PCIe 读 ~11 / 写 ~14 GB/s(写 > 读),低于 die 对 HCCS 17–19 GB/s(02),符合链路预期。
- **4→1 单向慢读**:仅 1.29 GB/s(~9 倍差),两次复跑一致;正确性 OK,属带宽级单向不对称。
- **同设备 0→0 FAIL**:PEER_ACCESS import ret=0 但数据通路坏;ENABLE_PEER_ACCESS 仅适用跨卡,同设备走 DEFAULT(02 已证)。
- **D 态挂死已修复**:ResetDevice 前 `DisablePeerAccess` 拆 P2P,运行后无 D 状态残留。

## 关联测试
- **15_peer_needed**:跨卡裸 DEFAULT import 是否需要 PEER 的干净对照。本测试 step2 已 PEER import,P2P 状态 sticky 干扰后续 DEFAULT,故该问题由 15 在独立进程专门测试。
