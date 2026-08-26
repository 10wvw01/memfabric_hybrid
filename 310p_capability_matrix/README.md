# 0824-test — 310P3 GVA 内存池能力矩阵测试套件

> 机器:Atlas 300I Duo(310P3)× 4 卡 = 8 dev / CANN 9.1.0 / aarch64
> 目标:逐项验证 GVA 内存池的建立(IPC/VMM/host)、数据传输(H2D/D2H/D2D)、
> 跨进程共享能力,每项一个独立可运行目录,正反对照,结论回填 README。

## ⚠ 运行前必读:D 态挂死根因与防护

### 问题
跨卡 P2P 测试(`aclrtDeviceEnablePeerAccess`)在**进程退出时**会触发驱动级死锁:
```
exit_mmap → __mmu_notifier_release → mn_hlist_release → synchronize_srcu
```
进程永久卡在 D 状态(SIGKILL 不可回收),残留设备内存 + hugepages 不释放。
多次运行累积 → hugepages 耗尽(18258/18258)→ 后续测试全部挂死 → 只能重启机器。

### 根因(三条证据链)
1. **02_ipc_die vs 03_ipc_cross**:cleanup 结构完全相同,唯一差异是 02 无
   EnablePeerAccess(die 对 DEFAULT)、03 有(跨卡 PEER flag)→ 02 正常 / 03 挂死;
2. **03 vs gva_pool.c**:都做跨卡 P2P,但 03 用 IPC(IpcMemClose 不 unmap VA)挂死,
   gva_pool 用 VMM(UnmapMem 显式摘 VA)正常;
3. `aclrtDeviceDisablePeerAccess` 符号存在(acl_rt.h:2969)但旧版从未调用。

### 修复(已应用到全部涉及 P2P 的测试:02~05/07~15)
所有跨卡测试的 cleanup 在 `aclrtResetDevice` 前加 `aclrtDeviceDisablePeerAccess(peer)`,
显式拆除 P2P 状态(未 enable 过时返回 507899,best-effort 无害,如 05/09)。
所有 run.sh 加 timeout 兜底(60s;07_d2d_matrix 与 310p_hcomm_demo 为 120s)。

### 运行纪律
1. **每次运行后检查**:`ps aux | grep test | grep -v grep`——不应有 D 状态残留
2. **若出现 D 态**:立即停止,不要继续跑后续测试(hugepages 会耗尽);
   需要 `npu-smi set` 在**宿主机**(非容器)复位设备,或重启
3. **运行前检查 hugepages**:`npu-smi info | grep Hugepages`——应远低于 18258/18258
4. **跨卡测试单次运行**:不要在 D 态残留未清理时连续跑

## 测试矩阵

| # | 目录 | 能力 | 规模 | 状态 |
|---|---|---|---|---|
| 01 | 01_base_memcpy | 设备内存 + H2D/D2H/同设备D2D | 1 NPU | ✅ PASS |
| 02 | 02_ipc_die | IPC die 对(DEFAULT✓/PEER✗/sync✗) | 2 NPU | ✅ PASS |
| 03 | 03_ipc_cross | IPC 跨卡(PEER✓/DEFAULT sticky/✗单元格) | 2 NPU | ✅ PASS(已修 D态) |
| 04 | 04_vmm_cross | VMM 跨卡(EnableP2P✓/ctx技巧✗→实测可用) | 2 NPU | ✅ PASS(已修 D态) |
| 05 | 05_vmm_die | VMM die 对 ctx 技巧 | 2 NPU | ✅ PASS |
| 06 | 06_host_pool | host DRAM 跨进程池(POSIX shm,4种PCIe配置) | 2 进程 1 NPU | ✅ PASS |
| 07 | 07_d2d_matrix | 8 NPU IPC 池全链路(受P2P槽位限制) | 8 NPU | ⚠️ FAIL(每卡8槽位硬约束,4-rank可行) |
| 08 | 08_host_swap | host 中转兜底(D2H+H2D,3组配置) | 2 NPU | ✅ PASS |
| 09 | 09_vmm_samedev | VMM 同设备跨进程 | 2 进程 1 NPU | ✅ PASS |
| 10 | 10_vmm_hardcell | VMM ✗ 单元格边界(已推翻) | 2 NPU | ✅ PASS(推翻预期,VMM 可达) |
| 11 | 11_ipc_va_determinism | IPC VA 确定性(3进程) | 3 进程 2 NPU | ✅ PASS |
| 12 | 12_topo_probe | CanAccessPeer + EnablePeer 矩阵 | 8 NPU | ✅ PASS(已修 D态) |
| 13 | 13_bw_compare | IPC vs VMM vs host-swap 带宽深度分析(56组) | 8 NPU | ✅ PASS |
| 14 | 14_transfer_remote | 远端 slab 直达 H2D/D2H(pinned vs pageable) | 2 NPU | ✅ PASS |
| 15 | 15_peer_needed | 跨卡 IPC 是否需要 enable peer(干净对照) | 2 NPU | ✅ PASS |
| 16 | 310p_hcomm_demo | hcomm 风格 IPC 复现(DEFAULT flag 边界) | 4 NPU | ✅ 完成(die pair 4/4 OK;跨卡 DEFAULT 8/8 FAIL=平台边界,见 15) |

## 运行方法

```bash
# 单个测试(每个目录独立可运行)
cd 01_base_memcpy && ./run.sh

# 检查 D 态残留(每次跨卡测试后必做)
ps aux | grep test | grep -v grep

# 检查 hugepages(运行前)
npu-smi info | grep Hugepages
```

## 已验证的核心结论(v3,全部测试完成)

### 一、建池与传输能力矩阵

| 链路 | 建池方式 | 传输 | 实测带宽 |
|---|---|---|---|
| die 对(0,1)等 | IPC DEFAULT | async D2D | 17.3~19 GB/s(02 三轮实测) |
| die 对(0,1) | VMM ctx 技巧 | async D2D | 读 19.0 / 写 17.9 GB/s(05) |
| 跨卡(48 条有向,13 全测通) | VMM EnableP2P | async D2D | Pull 1.2~12.4 GB/s(由 dst die 决定,见§二) |
| 跨卡 | IPC PEER | async D2D | Pull 1.2~12.4 GB/s(与 VMM 等价,13);Push ~13 GB/s |
| 同设备跨进程 | VMM | async D2D | ~88 GB/s(09) |
| 同设备跨进程 | IPC DEFAULT | async D2D | 37.3 GB/s(出处:memfabric-310p-plan 旧探针;本套件 09 仅测 VMM,未复测) |
| host 中转兜底 | IPC + D2H+H2D | sync | 1.8~3.9 GB/s(保底路径,08/13) |

### 二、跨卡带宽规律(56 组 Pull 模式实测,13_bw_compare;Push 模式另有补充测试)

| 场景 | dst=die0(主) | dst=die1(副) | 差异倍数 |
|------|-------------|-------------|---------|
| die 间(卡内 HCCS) | ~19 GB/s | ~19 GB/s | 1x(无差异) |
| 同 socket 跨卡 | ~11-12 GB/s | ~4 GB/s | 3x |
| 跨 socket | ~11-12 GB/s | ~1.25 GB/s | 9x |

**根因**:die0 是 PCIe 主 die,数据先到 die0 再经 HCCS 转发到 die1;Pull(读)模式需等 Completion TLP,dst=die1 时额外 HCCS 往返导致延迟增加、DMA 流水线气泡;Push(写)模式无此问题(~13 GB/s 一致)。

### 三、硬件约束

| 约束 | 值 | 影响 |
|------|-----|------|
| 每卡 P2P 槽位 | 8(两 die 共享) | 8-rank 全连接不可达(需 12 槽位),4-rank 可行 |
| IPC flag | DEFAULT=同卡 die pair,PEER=跨卡 | 不可互换,15_peer_needed 干净验证 |
| sync D2D | 共享内存(IPC/VMM imported)上不可用(507899/207000) | 共享链路必须 async MemcpyAsync + StreamSync;单进程自有设备内存 sync D2D 可用(~5 GB/s,01) |
| pinned vs pageable host | 2.86x 带宽差 | 远端 D2H:pinned 12.78 vs pageable 4.47 GB/s |

### 四、关键发现

1. **VMM 跨卡比预期宽松**(04):310P3 上 EnablePeerAccess 不是 Import 必要条件,ctx 技巧跨卡也可用
2. **✗ 单元格已推翻**(10):CANN 9.1.0 下 dev1→dev4 VMM import 可达,原假设不成立
3. **IPC VA 部分确定性**(11):importer 间 VA 一致,但 exporter 自 import 得到不同 VA
4. **host DRAM 池完全可用**(06):POSIX shm 跨进程共享,4 种 PCIe 配置全 PASS;x16 卡带宽(x2)是 x8 的 1.9 倍
5. **hcomm 风格 DEFAULT flag IPC 跨卡不可用**(310p_hcomm_demo):严格复现 hcomm(910 legacy)实现,
   die pair 4/4 OK、跨卡 8/8 FAIL(507899),与 15_peer_needed 结论互证;另发现容器内 bare PID ≠ getpid
