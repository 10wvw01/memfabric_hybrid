# 15_peer_needed — 跨卡 IPC 是否需要 enable peer(干净对照)

Atlas 300I Duo (310P3) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 2)。

## 目的
干净回答 **"跨卡 IPC import 是否需要 enable peer"**。

03_ipc_cross 的 step2 已 PEER import,同进程 P2P 状态 sticky 干扰后续 DEFAULT 判定;
本测试每次运行只测一个 MODE,进程独立,无先前 PEER 状态干扰。

## 三个 MODE(环境变量 `MODE`,每次独立 mpirun)

| MODE | importer 行为 | 结果 |
|---|---|---|
| `DEFAULT` | `ImportByKey(key, DEFAULT)`,不做任何 enable | 507899 |
| `ENABLE` | 先 `aclrtDeviceEnablePeerAccess(A_DEV)`,再 `ImportByKey(key, DEFAULT)` | 仍 507899 |
| `PEER` | `ImportByKey(key, ENABLE_PEER_ACCESS)` | import 0 + D2D OK |

import 成功后追加 `async D2D 读 + D2H 逐字校验`,验证数据通路真实可用。

## 运行
```
./run.sh                  # 依次跑 DEFAULT/ENABLE/PEER
MODE=DEFAULT A_DEV=0 B_DEV=2 mpirun --allow-run-as-root -np 2 ./test  # 单跑一个 MODE
```

## 结论

**跨卡 IPC import 必须用 `ENABLE_PEER_ACCESS` flag;DEFAULT flag 跨卡必然 507899,即使先显式 `aclrtDeviceEnablePeerAccess` 成功也不行。**

三个 MODE 全部符合预期(2026-08-25,默认 0→2,exit=0,无 D 状态残留):
- **DEFAULT → 507899**:从未做任何 PEER/enable,裸 DEFAULT 跨卡必然失败。
- **ENABLE → 507899**:`EnablePeerAccess` ret=0 但 DEFAULT import 仍失败——显式 enable 不能替代 PEER flag,DEFAULT import 路径本身不支持跨卡。
- **PEER → import 0 + D2D OK**:PEER flag import 是跨卡唯一可行方式,内部做的远不止 EnablePeerAccess(还建立跨卡 VA 映射等)。

**修正 03 旧版 sticky 归因**:03 旧 step3 的"PEER 后 DEFAULT 可用"不是 P2P 使能 sticky(本测试 ENABLE 模式证明单独 enable 不够),而是 PEER import 建立的完整跨卡 VA 映射状态 sticky。
