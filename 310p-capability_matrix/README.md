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

### 根因与修复(本套件内证据)
1. **02_ipc_die vs 03_ipc_cross**:cleanup 结构完全相同,唯一差异是 02 无
   EnablePeerAccess(die 对 DEFAULT)、03 有(跨卡 PEER flag)→ 02 正常 / 03 旧版挂死;
2. **修复**:cleanup 在 `aclrtResetDevice` 前显式 `aclrtDeviceDisablePeerAccess(peer)`
   对称拆除 P2P 状态;03/04/07/10/12/16 修复后实测均干净退出、无 D 态残留;
3. **05_vmm_die 不调用 EnablePeerAccess(ctx 技巧)**,天然无 D 态风险。

### 运行纪律
1. **每次运行后检查**:`ps aux | grep test | grep -v grep`——不应有 D 状态残留
2. **若出现 D 态**:立即停止,不要继续跑后续测试(hugepages 会耗尽);
   需要 `npu-smi set` 在**宿主机**(非容器)复位设备,或重启
3. **运行前检查 hugepages**:`npu-smi info | grep Hugepages`——应远低于 18258/18258
4. **跨卡测试单次运行**:不要在 D 态残留未清理时连续跑

## 测试矩阵

| # | 目录 | 能力 | 规模 | 状态 |
|---|---|---|---|---|
| 01 | 01_base_memcpy | 设备内存 + H2D/D2H/同设备D2D(sync/async, pinned/pageable) | 1 NPU | ✅ PASS |
| 02 | 02_ipc_die | IPC die 对(DEFAULT✓/PEER✗/sync✗ 对照) | 2 NPU | ✅ PASS |
| 03 | 03_ipc_cross | IPC 跨卡(PEER✓/同设备PEER✗ 对照) | 2 NPU | ✅ PASS(已修 D态) |
| 04 | 04_vmm_cross | VMM 跨卡(EPA 非必须✓/ctx 技巧✓ + memAttr 矩阵) | 2 NPU | ✅ PASS(已修 D态) |
| 05 | 05_vmm_die | VMM die 对 ctx 技巧(本进程 ctx✗ 对照) | 2 NPU | ✅ PASS |
| 06 | 06_host_pool | host DRAM 跨进程池(POSIX shm,4 卡配置 + host register 边界) | 2 进程 1 NPU | ✅ PASS |
| 07 | 07_d2d_matrix | 8 NPU IPC 池全链路 + P2P 槽位配额精确定位 | 8 NPU | ⚠️ FAIL(每卡 8 槽位硬约束,4-rank 实测可行) |
| 08 | 08_host_swap | host 中转兜底(D2H+H2D,3 组配置) | 2 NPU | ✅ PASS |
| 09 | 09_vmm_samedev | VMM 同设备跨进程(sync D2D✗ 对照) | 2 进程 1 NPU | ✅ PASS |
| 10 | 10_vmm_hardcell | VMM 跨卡/跨 die 三种 import 方式(✗ 单元格已推翻) | 2 NPU | ✅ PASS |
| 11 | 11_ipc_va_determinism | IPC VA 确定性(3 进程) | 3 进程 2 NPU | ✅ PASS |
| 12 | 12_topo_probe | CanAccessPeer + EnablePeer 8×8 矩阵 | 8 NPU | ✅ PASS(已修 D态) |
| 13 | 13_bw_compare | IPC vs VMM vs host-swap 带宽深度分析(56 组 Pull + Push 补充) | 8 NPU | ✅ PASS |
| 14 | 14_transfer_remote | 远端 slab 直达 H2D/D2H(pinned vs pageable) | 2 NPU | ✅ PASS |
| 15 | 15_peer_needed | 跨卡 IPC 是否需要 enable peer(5 MODE 干净对照) | 2 NPU | ✅ PASS |
| 16 | 16_vmm_matrix | 8 NPU VMM 池全连接(对照 07 IPC 版) | 8 NPU | ⚠️ FAIL(VMM import 隐式 enable P2P,同一配额墙,失败矩阵与 07 全同;VA 槽位窗口 CONSISTENT) |

## 运行方法

```bash
# 单个测试(每个目录独立可运行;run.sh 内部先 make 再运行,二进制已清理可随时重建)
cd 01_base_memcpy && ./run.sh

# 检查 D 态残留(每次跨卡测试后必做)
ps aux | grep test | grep -v grep

# 检查 hugepages(运行前)
npu-smi info | grep Hugepages
```

## 已验证的核心结论(全部来自 01-16 实测)

### 一、建池与传输能力矩阵

| 链路 | 建池方式 | 传输 | 实测带宽 |
|---|---|---|---|
| die 对(0,1)等 | IPC DEFAULT | async D2D | 读 18.9 / 写 17.5 GB/s(02,三轮稳定 17-19) |
| die 对(0,1) | VMM ctx 技巧 | async D2D | 读 19.00 / 写 17.94 GB/s(05) |
| 跨卡(56 有向对全测) | VMM import(零显式 EPA) | async D2D | Pull 1.25~12.4 GB/s(dst die 决定,见§二)(13) |
| 跨卡 | IPC PEER | async D2D | 读 ~11 / 写 ~13-14 GB/s(03);与 VMM 性能等价(13 全测) |
| 同设备跨进程 | VMM | async D2D | ~88 GB/s(09) |
| 单进程同设备 | aclrtMalloc | async D2D | 65 GB/s(16MB),渐近 ~91 GB/s(256MB)(01) |
| host 中转兜底 | IPC + D2H+H2D | sync | 2.75~3.21 GB/s(08,任何链路保底) |

另:host 直达(imported VA 上直接 H2D/D2H sync)在 die 对 / 跨卡 / 同设备全部可用并逐字校验通过(02/03/04/05/09/14)。

### 二、跨卡带宽规律(56 组 Pull 实测 + Push 补充,13_bw_compare)

| 场景 | dst=die0(主) | dst=die1(副) | 差异倍数 |
|------|-------------|-------------|---------|
| die 间(卡内 HCCS) | ~19 GB/s | ~19 GB/s | 1x(无差异) |
| 同 socket 跨卡 | ~11-12 GB/s | ~4 GB/s | 3x |
| 跨 socket | ~11-12 GB/s | ~1.25 GB/s | 9x |

- **根因**:die0 是 PCIe 主 die,数据先到 dst die0 再经 HCCS 转发到 die1;Pull(读)是
  Non-Posted 事务需等 Completion TLP,dst=die1 时额外 HCCS 往返导致 DMA 流水线气泡;
  Push(写)是 Posted 事务无需等待,dst=die0/die1 均 ~13 GB/s 一致(13 Push 实测:
  0→3 由 4.17 提升至 12.95,0→5 由 1.29 提升至 13.08)。
- **带宽由 dst die 决定,src die 无影响**(13 全矩阵;11 亦证 dev1→dev3 = 3.98 GB/s)。
- **单向不对称实例**:4→1 读仅 1.29 GB/s(写 13.00),两次复跑一致(03)。
- PCIe 链路本身不是瓶颈(效率 73-83%);跨 socket 与同 socket 跨卡 dst=die0 带宽相同。
- IPC 与 VMM 两种后端带宽逐组一致(13)。

### 三、硬件约束

| 约束 | 值 | 影响 | 出处 |
|------|-----|------|------|
| 每卡 P2P 槽位 | 8(两 die 共享) | 8-rank 全连接不可达(每卡需 12 条跨卡关系);VMM 同样绕不过(16 实测失败矩阵与 07 逐格相同);4-rank 全连接实测可行;6-rank(2+2+1+1)与 ring/doubling 稀疏拓扑为 07 分析可行 | 07/16 |
| 槽位计费粒度 | 跨卡关系条数,非字节 | SLAB_MB=2/16/64 三组失败矩阵逐条一致;零内存的纯 EnablePeerAccess 探针同样触顶(正序第 37 条起 507899);4 卡最终恰好 8/8 饱和(16 条跨卡关系/32 条 import) | 07/16 |
| die 对 import 不占槽 | phy==peer 跳过 EnableP2P | die 对 ctx 技巧 import 零扣费;但全连接每卡仍需 12 条跨卡 > 8,die 对免费也救不了 | 16 |
| IPC flag 适用域 | DEFAULT=同卡 die 对,PEER=跨卡 | 不可互换(02:die 对 PEER 507899;07:跨卡 DEFAULT 全 FAIL;03:同设备 PEER import ret=0 但数据通路坏) | 02/03/07 |
| 跨卡 IPC 必要条件 | 双向 P2P 均使能 | PEER flag 自动双向;DEFAULT flag 需两端各调一次 EnablePeerAccess(ENABLE_BOTH 实测 OK);任一单向都不够(507899) | 15 |
| 同卡跨 die EnablePeerAccess | 507899 | die 对不被 runtime 识别为合法 P2P 对;EnablePeerAccess 也无法替代 VMM die 对的 ctx 技巧 | 05/10/12 |
| sync D2D | 共享/远端内存上不可用 | die 对 IPC 507899(02)、VMM 同设备 207000(09);共享链路必须 MemcpyAsync + StreamSync;单进程自有设备内存 sync D2D 可用 ~5 GB/s(01) | 01/02/09 |
| pinned vs pageable | 2.86x 带宽差 | 远端 D2H:pinned 12.78 vs pageable 4.47 GB/s(14);本地 H2D 2.9x / D2H 4.3x(01);pageable 走驱动 staging 两次拷贝 | 01/14 |
| PCIe lane 数 | x16 ≈ 1.9× x8 | H2D:x16 卡 26.9-26.92 / x8 卡 13.78-13.93 GB/s(06 四卡实测);跨卡 P2P 带宽与 lane 数无关(13) | 06/13 |

### 四、API 行为规律

1. **VMM memAttr(04 test_memattr 实测)**:310P3 支持 6 种——`ACL_MEM_NORMAL`/
   `ACL_MEM_HUGE`/`ACL_MEM_P2P_NORMAL`/`ACL_MEM_P2P_HUGE` + `ACL_HBM_MEM_HUGE`/
   `ACL_HBM_MEM_NORMAL`(后两种与官方文档"仅 350/A3/A2"不符);1G 大页 207000;
   HBM_P2P 组合与 DDR 前缀 100000。七级回退链实际命中 `ACL_MEM_P2P_HUGE`,
   granularity=2MB(04/05/09/16 一致)。
2. **VMM import 三种方式(10)**:跨卡——本进程 ctx 直接 import 即可,EnablePeerAccess
   非必要条件(04 step16);同卡跨 die——本进程 ctx 507899,必须 ctx 技巧
   (`SetDevice(A)`→`Import(sh,B)`→`SetDevice(B)`)或 `aclrtCreateContext(A)` +
   `SetCurrentContext`(方式三,与方式二等效且 ctx 生命周期显式可控)。机制:import
   内部用当前 ctx 设备调 `EnableP2PWithOtherDevice`,ctx=导出方时跳过 P2P 启用。
3. **IPC VA 部分确定性(11)**:importer 间 VA 完全一致(跨卡 PEER + 同设备 DEFAULT)
   且等于 exporter 的 malloc 地址;exporter 自 import 得到不同 VA。IPC 按成功 import
   顺序分配 VA,不同 rank 顺序不同则矩阵错位(07);VMM + gva_pool 式槽位窗口
   (每 rank Reserve 128MB,slot p=base+p×16MB)按构造保证全网同 VA(16 实测 CONSISTENT)。
4. **host register 全系列不可用(06)**:`aclrtHostMemMapCapabilities` 10 种 HAC 类型
   全 NOT_SUPPORTED;`aclrtHostRegister` V1/V2 MAPPED 207000;V2 PINNED ret=0 但为
   空操作(仅 RT 层记录区间,无驱动调用);`aclrtHostGetDevicePointer` 107000;
   `halHostRegister` flag=0→8 / flag=3→65534。310P3 无法实现设备直接访问 host 内存。
5. **拓扑 API(12)**:CanAccessPeer 8×8 对称——die 对 can=0(4/4),跨卡 can=1(24/24);
   EnablePeerAccess——die 对 507899(8/8 有序对),跨卡全部成功(48/48)。每对测试后
   立即 disable 可消除 sticky 状态污染。
6. **拷贝 API 路径(01)**:H2D/D2H pinned 下 sync 反比 async 快 1.4×(26.96/19.13、
   24.22/17.75 GB/s,async 提交/同步开销抵消流水线收益);D2D sync 走 drvMemcpy
   ~5 GB/s,async 走 SDMA 65~91 GB/s,差距 12.6×(sweep 4-256MB 单调扩大)。
   H2D/D2H 推荐 sync + pinned;D2D 必须 async。

### 五、关键发现

1. **8-rank 全连接在本平台双路径均不可达(07/16)**:每卡 8 个入向 P2P 授权槽位
   (TX ATU 16 窗口 ÷ 每连接 2 = 8,驱动注释 "Ascend310P Duo 两 die share 8"),
   IPC `ENABLE_PEER_ACCESS` 与 VMM import(内部隐式 `halDeviceEnableP2P`,
   runtime `api_impl.cc` → `EnableP2PWithOtherDevice`)消耗同一配额,失败矩阵逐格
   相同。替代方案:4-rank 全连接(07 实测 join/write 全 OK)、ring(每卡 2 槽)/
   doubling(每卡 6 槽)稀疏拓扑(07 分析)、VMM 槽位窗口(需全网同 VA 时)。
2. **✗ 单元格假设已推翻(10)**:CANN 9.1.0 下 dev1↔dev4 VMM import 实际可达
   (ret=0,三种方式全成功);IPC-PEER 该链路也一直可用(03 实测 11.25/13.14 GB/s)。
3. **VMM 跨卡比预期宽松(04)**:EnablePeerAccess 不是跨卡 Import 的必要条件;
   ctx 技巧(本为 die 对设计)跨卡也能用但非必须。
4. **host DRAM 池完全可用(06)**:POSIX shm(MAP_SHARED)跨进程共享同物理页,
   跨进程写入对端 H2D 立即可见;共享 host 内存可直接作 `aclrtMemcpy(H2D)` 源,
   4 卡配置全 PASS。
5. **远端直达与本地无差异(14)**:imported VA(远端 die)→ pinned host 12.78 vs
   本地 dev → pinned host 12.84 GB/s,数据路径同为 die0→PCIe→host;pageable 是
   唯一瓶颈(2.86×)。
6. **Push 模式可绕过 dst=die1 读瓶颈(13)**:跨卡写 dst die0/die1 均 ~13 GB/s;
   性能建议——跨卡优先 dst die0 接收、必须写 die1 时用 Push(src 主动写)、
   避免跨 socket + dst=die1 + Pull 最差组合(~1.25 GB/s);关键数据 Push 后补校验
   (Posted 写的 ACK 不保证 HCCS 转发落地)。
7. **507899 = `ACL_ERROR_RT_DRV_INTERNAL_ERROR`**(rt_error_codes.h:148):驱动对
   不支持路径的确定性回复而非随机错误——die 对 PEER import、跨卡 DEFAULT import
   (无双向 P2P)、共享内存 sync D2D、同卡跨 die EnablePeerAccess、超槽位 enable 均返回它。
