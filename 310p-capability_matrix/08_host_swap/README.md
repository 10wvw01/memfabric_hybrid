# 08_host_swap — host 中转兜底(D2H + H2D 跨设备)

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(`A_DEV`,默认 0),rank1=importer(`B_DEV`,默认 1 die 对)。
同卡 die 对用 DEFAULT import,跨卡用 PEER import。

## 测试目的

验证 **host 中转兜底路径**:当设备间直达传输不可用时(同步路径失败),通过 `远端设备内存
→ host 内存 → 本地设备内存` 的 D2H + H2D 两跳搬运仍能完成跨设备数据传输,并量化该兜底
路径与 direct D2D 的带宽差距。(注:原假设的 ✗ 单元格 VMM 不可达已被 10 推翻)

## 测试内容

1. **exporter 准备**:malloc 32MB P2P 内存 + 写入校验 pattern + 导出 IPC key 发给 importer
2. **importer 导入**:import key(die 对 DEFAULT / 跨卡 PEER)获得远端内存 VA + 分配本地 32MB
3. **host-swap 读路径**:D2H(远端→host) + H2D(host→本地) + 逐字校验 pattern
4. **host-swap 写路径**:填本地 pattern → D2H(本地→host) + H2D(host→远端) → exporter 端校验
5. **带宽对比**:同链路 direct async D2D vs host-swap,均按 3 预热 + 30×16MB async + 末尾 1 次 Sync 口径

校验 pattern:`(rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 0x8/0x9 每步不同,防残留数据假阳性。

## 测试结果(2026-08-25,三组配置全 PASS,无挂死)

| 链路 | 步骤 1-4 | direct async D2D | host-swap | RESULT |
|---|---|---|---|---|
| die 对 dev0→dev1 | 全 OK | 19.02 GB/s | 3.03 GB/s(6.3×) | PASS |
| 跨卡 dev0→dev2 | 全 OK | 11.32 GB/s | 3.21 GB/s(3.5×) | PASS |
| ✗ 单元格 dev1→dev4 | 全 OK | 11.45 GB/s | 2.75 GB/s(4.2×) | PASS |

## 关键结论

1. **host-swap 功能完全可用**:读写双向逐字校验全部通过,~3 GB/s 是任何 IPC 链路上的
   保底传输能力
2. **带宽代价明确**:die 对上损失约 6 倍,跨卡上约 3.5-4 倍
3. **✗ 单元格真相(已更新)**:原假设 dev1→dev4 为 VMM 不可达 ✗ 单元格,但 10_vmm_hardcell
   实测(CANN 9.1.0)显示 VMM import 实际可达(ret=0)。IPC-PEER import 和 async direct D2D
   一直可用;host-swap 仍为兜底路径,但 VMM 场景下不再需要兜底
4. **D 态修复有效**:跨卡时 ResetDevice 前 DisablePeerAccess,importer 侧 ret=0
   (exporter 侧 ret=507899,本侧未启用过 peer,预期内无害),进程干净退出

## API 链
- export:`aclrtMalloc(HUGE_FIRST_P2P)` 32MB → H2D pattern →
  `aclrtIpcMemGetExportKey(DISABLE_PID_VALIDATION)` → key 经 MPI 发出
- import:`aclrtIpcMemImportByKey`(die 对 DEFAULT / 跨卡 PEER_ACCESS)
- cleanup:双方 `aclrtIpcMemClose`;物理内存仅 exporter `aclrtFree`;
  跨卡时 ResetDevice 前 `aclrtDeviceDisablePeerAccess`(D 态修复)

## 带宽口径
3 预热 + 30×16MB MemcpyAsync 提交 + 末尾 1 次 StreamSync。**严禁同步循环测带宽**
(失败链路同步调用 ~140µs/次会伪造 114 GB/s 假带宽)。host-swap 按有效交付
30×16MB=480MB 与 direct 同口径对比(总线流量 2×)。

## 运行方法
```
./run.sh                    # 默认 die 对 dev0→dev1
A_DEV=0 B_DEV=2 ./run.sh    # 跨卡(PEER import + D 态修复)
A_DEV=1 B_DEV=4 ./run.sh    # 原 ✗ 单元格(10 实测 VMM 已可达)
```
