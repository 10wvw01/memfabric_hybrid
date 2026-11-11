# 16_vmm_matrix — 8 NPU VMM 内存池全连接

Atlas 300I Duo(310P3,4 卡 × 2 die = 8 dev)/ CANN 9.1.0 / aarch64,MPI 8 进程,
rank k = dev k(恒等映射)。die 对 = (0,1)(2,3)(4,5)(6,7)。

## 动机(对照 07_d2d_matrix)

07 已证明 **IPC 方式 8-rank 全连接不可达**:每卡 8 个入向 P2P 授权槽位
(4 卡 × 8 = 32 上限),全连接需 card0 参与 12 条跨卡关系(12 > 8),
`ENABLE_PEER_ACCESS` flag import 内部的 `halDeviceEnableP2P` 必然触顶(507899)。

本测试验证:**VMM shareable handle 路径全程不调用 `EnablePeerAccess`**
(04 step16 实测跨卡 import 不依赖 EPA;05 实测 die 对用 ctx 技巧无需 EPA),
是否能绕过 P2P 槽位配额,实现 56 条有向链路全连接。

## 建池方式(gva_pool 同款槽位窗口)

每 rank 预留 128MB VA 窗口(N × 16MB slot),`slot p = base + p*16MB`:
- own 物理内存 `MapMem` 到 slot r
- import 的各 peer 物理内存 `MapMem` 到 slot p
- → 所有 rank 的 slot 布局一致(VA 矩阵按构造对称)

## 验证能力

- 8 rank VMM 池建池(`MallocPhysical` 七级 memAttr 回退 → `ReserveMemAddress`
  128MB → `MapMem` own@slot r → `ExportToShareableHandle`)
- join(token 环串行 import,56 条有向链路,**零 EnablePeerAccess**)
  - method1 本进程 ctx:`ImportFromShareableHandle(sh, my_dev)`(507899 时 200ms 重试 1 次)
  - method2 ctx 技巧(仅 method1 失败时):`SetDevice(p)` → `Import(sh, my_dev)` → `SetDevice(my_dev)`
- 全网写+校验(每 rank 写 1 peer `p=(r+1)%8` + D2H 逐字校验被写 pattern)
- VA 矩阵一致性(OBS,slot 布局;NULL-hint reserve 下 base 是否跨进程一致是平台观测值)

## API 链

- **create**:`aclrtMallocPhysical(16MB, 七级回退, 310P3 仅 ACL_MEM_P2P_HUGE 成功,
  gran=2MB)` → `aclrtReserveMemAddress(128MB, NULL)` → `aclrtMapMem(own@slot r)` →
  `aclrtMemSetAccess(RW, dev)` → H2D pattern(rank, tag=0x16) →
  `aclrtMemExportToShareableHandle(DISABLE_PID_VALIDATION)`
- **exchange**:`MPI_Allgather` 8 个 sh(u64)
- **join**:token 环串行,rank r import 7 个 peer → `MapMem(slot p)` → `SetAccess(RW, r)`
- **write**:每 rank r H2D pattern(tag=0x60+r) → scratch → `MemcpyAsync(D2D, 16MB)` →
  slot (r+1)%8 → `SynchronizeStream` → Allgather delivered → D2H own slot 逐字校验
- **bandwidth**(正确性全过后):token 环串行,die 对(r^1) + 跨卡((r+2)%8)各
  3 预热 + 30×4MB async + 1 Sync
- **cleanup**:UnmapMem(import slots) → FreePhysical(imported) → Barrier →
  UnmapMem(own) → FreePhysical(own) → ReleaseMemAddress → DestroyStream →
  DisablePeerAccess ×7(best-effort,从未 enable 预期 507899 无害)→
  ResetDevice(+ctx 技巧额外开的 dev)→ Finalize

校验 pattern:`value = (rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 每步不同
(0x16 建池 / 0x60+r 写)防残留假阳性。

## 运行方法

```
./run.sh                            # 8 NPU, 16MB slab, timeout 120s
SLAB_MB=2 ./run.sh                  # 2MB slab(最小粒度, 排除内存大小假设的对照)
SLAB_MB=64 ./run.sh                 # 64MB slab
make && ./test                      # 等价(默认 SLAB_MB=16)
```

## 预期结果

- 8 rank create:MallocPhysical + Reserve + Map + Export 全 OK
- join:8 rank 各 7/7(56 条有向链路全通)—— **VMM 不消耗 P2P 槽位则可达**
- 全网写+校验:8 条 D2D 写 + D2H 逐字校验全 OK
- VA 矩阵:slot 布局一致;base 跨进程是否相同为观测项
- `RESULT: PASS (join:OK write:OK VA:CONSISTENT)`
- 若 join 出现 507899 集中于某些 rank/peer → VMM 同样受槽位限制(记录分布,
  对照 07 的失败矩阵);若 import 失败但错误码不同 → 如实记录分析

## 判定

PASS = (8 rank join 全 7/7)&&(写校验全 OK)。VA 一致性仅 OBS 报告
(NULL-hint reserve 的 base 由各进程 VA 分配器决定,不构成构造保证)。

## ★实测结论(2026-08-26 运行,两次运行结果完全一致)

`./run.sh`(8 NPU,timeout 120s),退出码 exit=1(**干净 FAIL,非 124 超时,无 D 态残留**)。

### 结果速览

| 阶段 | 实测结果 | 期望 |
|---|---|---|
| ① create | 8/8 OK,MallocPhysical(ACL_MEM_P2P_HUGE)+Reserve 128MB+Map own@slot+Export sh 全 OK | OK |
| ② exchange | 8 sh allgather OK(sh=0x{rank}00000001) | OK |
| ③ join | r0/r2=7/7,**r1/r3=3/7,r4-r7=5/7** | 7/7 |
| ④ VA | **CONSISTENT**(slot 窗口构造,8 行完全一致) | 一致 |
| ⑤ write | 30 条成功链路写+校验全 OK;r3→p4 等 26 条 SKIP | 8/8 |
| ⑥ 带宽 | SKIP(正确性未过) | 跑通 |
| ⑦ cleanup | 全部干净释放,无挂死,exit=1 | 干净退出 |
| RESULT | `FAIL (join:FAIL write:FAIL VA:CONSISTENT)` | PASS |

### 与 04/05 路径一致性验证(实现正确性的交叉证据)

- **die 对 4/4 全部走 method2 ctx 技巧成功**(`SetDevice(p)`→`Import(sh,my_dev)`→`SetDevice(my_dev)`)
  ——与 05 验证的主路径一致;05 的对照实验(die 对 own-ctx import 确定性 507899)在首轮
  method1-first 版本中也复现(所有 die 对 own-ctx 均先 507899 再落到 ctx 技巧)。
- **跨卡 import 走 method1 本进程 ctx 无显式 EPA 全部在配额内成功**——与 04 step16 的
  "无 EnablePeerAccess 直接 Import 意外成功"一致;die 对/跨卡路径选择正是按 04/05 的
  已验证结论分工。**单条链路层面两种 import 全部可行,FAIL 完全来自卡级槽位配额**
  (见下方配额模型),不是 import 方法问题。
- 建池链(MallocPhysical 七级回退→Reserve→MapMem→SetAccess→H2D→Export DISABLE_PID)
  与 04/05 完全一致;唯一构造差异是 gva_pool 式 128MB 槽位窗口(Reserve 一次,
  每 peer MapMem 16MB 到固定 slot),运行验证可行(所有 map ret=0)。

### ★★核心结论:VMM 不能绕过 P2P 槽位配额 —— 与 IPC 失败矩阵完全相同

失败全部集中在跨卡 import 的 `507899`,**分布与 07 IPC 版逐格一致**:

| rank | 结果 | 失败的跨卡 peer |
|---|---|---|
| r0 (dev0) | 7/7 ✓ | 无 |
| r1 (dev1) | 3/7 | p4 p5 p6 p7 |
| r2 (dev2) | 7/7 ✓ | 无 |
| r3 (dev3) | 3/7 | p4 p5 p6 p7 |
| r4 (dev4) | 5/7 | p1 p3 |
| r5 (dev5) | 5/7 | p1 p3 |
| r6 (dev6) | 5/7 | p1 p3 |
| r7 (dev7) | 5/7 | p1 p3 |

**直接证据:VMM import 内部隐式 enable 了 P2P。**
本测试全程零显式 `EnablePeerAccess`,但 cleanup 的
`DisablePeerAccess`(best-effort)返回 ok 的数量与各 rank 跨卡 import 成功数
**完全相等**:r0=6、r2=6、r1/r3=2、r4-r7=4(从未 enable 过的 peer 返回 507899;
die 对 ctx 技巧 import 不产生 P2P 状态,disable 恒 507899)。
即 VMM 跨卡 import 与 IPC `ENABLE_PEER_ACCESS` flag import 一样,内部调用
`halDeviceEnableP2P`,消耗同一份每卡 8 个槽位配额。

### ★★★对照实验:排除"内存大小超限"假设(SLAB_MB 参数)

**问题**:失败会不会是每次 import 的 P2P 内存太大超出限制导致的?

**排除该假设的三重证据**:

1. **零内存也触顶**(结构性证据):`07_d2d_matrix/test_quota_probe.c` 只调
   `aclrtDeviceEnablePeerAccess/DisablePeerAccess` 设备级 API,**完全不分配任何
   P2P 内存**,照样在同一配额处触顶(36 条 enable,第 37 条起 507899)→ 限制的
   计数单位是**连接条数**,不是字节。
2. **同尺寸单条链路全通**(尺寸证据):`13_bw_compare` 用同样的 16MB VMM 块对
   全部 56 个有向对逐对测试(每对独立进程),**100% 成功**——包括全连接时失败的
   (1,4)(3,4)(1,6)(3,6) 等对。同一块内存,单测通过、并发建才失败,差异只在
   已建立的连接数。
3. **slab 大小 32 倍扫描矩阵不变**(控制变量实验,2026-08-26):
   `SLAB_MB` 环境变量参数化(2MB=最小粒度 / 16MB=默认 / 64MB),三组运行
   **失败矩阵逐条完全一致**(r1/r3 失 p4-p7;r4-r7 失 p1,p3;共 16 条 507899):

   | SLAB_MB | 2 | 16 | 64 |
   |---|---|---|---|
   | r0/r2 joined | 7/7 | 7/7 | 7/7 |
   | r1/r3 joined | 3/7 | 3/7 | 3/7 |
   | r4-r7 joined | 5/7 | 5/7 | 5/7 |
   | 失败 peer 集合 | 完全相同 | 完全相同 | 完全相同 |

   若限制与字节数相关,2MB(1/8 内存)应能多建若干条、64MB 应该更早失败;
   实测对 32 倍的尺寸变化完全不敏感。另注意组内不对称:r0 成功 import 了
   7×16MB=112MB,r1 只 import 到 3×16MB=48MB 就被拒——若是字节上限,应该
   先限制导入更多的 r0,而非更少的 r1。

**结论:与内存大小无关,是每卡 8 条 P2P 连接的条数硬配额。**
(运行方法:`SLAB_MB=2 ./run.sh` 或 `SLAB_MB=64 mpirun --allow-run-as-root -np 8 ./test`)

### ★★★精确配额模型(可完整解释每一个成功/失败)

**只有跨卡关系消耗槽位,die 对(同卡两 die)完全不占槽**:

- 一条**跨卡** import r→p 建立双向 P2P 关系 (dev_r, dev_p):两端卡各扣 1 个
  槽位;已存在的关系可复用(不重复扣费)。
- **die 对 import 零扣费**:runtime 源码 `EnableP2PWithOtherDevice`
  (raw_device.cc:2198)`if (phyDeviceId != peerPhyDeviceId)` —— 同一物理芯片
  的两个 die phy id 相同,**直接跳过** EnableP2P,不产生任何 P2P 状态。
- 每卡 2 die 共享的 8 个槽位,**全部由跨卡关系占用**;8/8 饱和 = 该卡两个 die
  的跨卡关系总数恰好填满 8 槽,die 对一条槽都不占。

die 对免费的三重证据:
1. **源码**:import 前置分支 `phy == peer` 跳过 EnableP2P(见上)。
2. **实测 DisablePeerAccess**:die 对 peer 的 disable 恒 507899(从未 enable 过)
   —— r0 的 ok=6/err=1,那 1 个 err 正是 die 对 p1;若 die 对占槽,ok 数会多 1。
3. **反事实算术**:若 die 对占 1 槽,r1 在 token1 时 p0(die)会把 card0 推到 8,
   p2 即失败 → r1 只能 1/7;实测 r1=3/7(p0 die + p2 + p3),只有 die 对免费
   才解释得通。

按 token 环顺序(r0→r7 串行 import)推演,与实测逐条吻合:

| token | rank | 结果 | 槽位演进 |
|---|---|---|---|
| 0 | r0 | 7/7 | 新建关系 (0,2)(0,3)(0,4)(0,5)(0,6)(0,7) → card0=6, card1=2, card2=2, card3=2 |
| 1 | r1 | 3/7(失 p4-p7) | (1,2)(1,3) 新建 → card0=8 **满**;p4 起需 card0 第 9 槽 → 507899 |
| 2 | r2 | 7/7 | p0/p1 **复用** (0,2)(1,2) 零扣费;(2,4)(2,5)(2,6)(2,7) 新建 → card1=8 **满** |
| 3 | r3 | 3/7(失 p4-p7) | p0/p1 复用;(3,x) 新关系需 card1 第 9 槽 → 507899 |
| 4-7 | r4-r7 | 各 5/7(失 p1,p3) | p0/p2 复用已有关系;(1,4)(3,4) 等新关系需 card0/card1 槽,两卡皆满 → 507899;(4,6)(4,7)(5,6)(5,7) 新建 → card2=card3=8 **满** |

**最终状态:4 张卡全部恰好 8/8 饱和,且 8 个槽全部是跨卡关系(die 对零参与)**
——共 16 条跨卡 P2P 关系、32 条跨卡 import 成功(48-32=16 条 507899),每张卡
的 8 个槽位全部用尽。各卡 8 条关系的构成(括号内为 dev 对,全部跨卡):

| 卡 | 8 条跨卡关系(共 16 条,每条两端各占 1 槽) |
|---|---|
| card0 (dev0,1) | (0,2)(0,3)(0,4)(0,5)(0,6)(0,7)(1,2)(1,3) |
| card1 (dev2,3) | (0,2)(0,3)(1,2)(1,3)(2,4)(2,5)(2,6)(2,7) |
| card2 (dev4,5) | (0,4)(0,5)(2,4)(2,5)(4,6)(4,7)(5,6)(5,7) |
| card3 (dev6,7) | (0,6)(0,7)(2,6)(2,7)(4,6)(4,7)(5,6)(5,7) |

16 条 × 2 端 = 32 = 4 卡 × 8,四个 die 对 (0,1)(2,3)(4,5)(6,7) 不出现在任何
一行。这不是部分失败,而是配额被精确榨干的饱和特征。

**全连接的数学**(注意只数跨卡关系,die 对免费):全连接需 24 条跨卡关系
(C(8,2)−4 个 die 对),每卡参与 2 die × 6 个远端 die = 12 条**跨卡**关系
(die 对不占槽,帮不上忙)→ 12 > 8,**拓扑本身超配额,与 import 方式、
顺序、IPC/VMM 后端均无关,数学上不可达**。

### ★★★驱动代码实证(/home/t00842504/cann-repo,2026-08-26 调研)

**① 官方注释直接写明限制**(pkg_inc/ascend_hal_base.h:1695-1720,halDeviceEnableP2P):

```
 * 1. Both directions must be set to take effect...
 * 2. When using device access host memory, the MAX enable number will be reduced.
 * 3. The two devices of Ascend310P Duo share 8 MAX enable number.
 * Chip Type        |   Support phy_id   | MAX enable number |
 * Ascend310P       |  0/2/4...34/36/38  |        8          |
 * Ascend310P Duo   |        0~39        |      8(shared)    |
```

与实测模型完全一致:两 die 共享 8 条;H2D 会挤占额度。

**② "为什么是 8"的代码级解释 —— 设备侧 TX ATU 窗口数**:
- `src/sdk_driver/comm/pcie/inc/devdrv_atu.h:32` `DEVDRV_MAX_TX_ATU_NUM = 16`(设备侧 TX ATU 总窗口)
- `devdrv_atu.h:36` `DEVDRV_SINGLE_DEV_TX_ATU_NUM = 2`(每条 mem P2P 连接占 2 窗口)
- **16 / 2 = 8 条连接/卡**,与注释 "8(shared)" 精确吻合
- `devdrv_atu.h:43` `MAX_H2D_ATU_NUM = 6`:H2D 与 P2P 共享同一 TX ATU 池
  → 注释第 2 条 "will be reduced" 的实现基础:
  **P2P(`ATU_TYPE_TX_MEM=3`,2 窗口/条,devdrv_ctrl.c:4290)与设备访问主机内存
  (`ATU_TYPE_TX_HOST=5`,1 窗口/块,devdrv_ctrl.c:5405)从同一 16 窗口池分配,
  且走同一条 admin 消息 `DEVDRV_CFG_TX_ATU` 下发固件**。若某卡有 pinned host
  内存注册给设备直接 DMA,每块占 1 窗口(上限 6 块),P2P 可用连接相应减少
  (极端 6 块 H2D → 仅剩 5 条 P2P)。本测试实测各卡恰好 8/8 饱和,说明运行时
  无 H2D 窗口占用(pageable malloc 走 host 中转不消耗 TX ATU),数字打满 8
  反向验证池子干净。

**③ 强制执行点在闭源固件**(主机侧只透传):调用链
`halDeviceEnableP2P`(src/ascend_hal/dms/drv_devmng/ascend910/devdrv_pcie.c:787)
→ `DmsEnableP2P`(dms/board/dms_p2p_attr.c:108,ioctl DMS_SUBCMD_DEV_P2P_ATTR)
→ `devdrv_enable_p2p`(sdk_driver/comm/pcie/host/devdrv_ctrl.c:4627,含 chip_type 分支)
→ `devdrv_add_p2p_tx_atu`(devdrv_ctrl.c:4132)→ **admin 消息 DEVDRV_CFG_TX_ATU
发给设备固件**;`devdrv_alloc_atu_id` 等分配函数在仓库中**只有声明无实现**,
固件拒绝后主机透传失败,runtime 统一包装为 507899(rt_error_codes.h:148,
ACL_ERROR_RT_DRV_INTERNAL_ERROR,通用包装码而非专用超限码)。

**④ 佐证**:310P Duo = PCI 0xd500 → HISI_MINI_V2(res_drv_mini_v2.c:603 日志
"devid and peer_devid is 1pf2die's die1, not support p2p");双向语义
devdrv_enable_p2p_inner(本端 ref==0 且对端 ref>0 才真正 cfg_enable)与
实测"一条跨卡 import = 两端卡各扣 1 槽"的双向关系模型吻合。

**⑤ 源码级确认:VMM import 内部确实调 halDeviceEnableP2P(runtime 层,非 HAL 层)**

CANN runtime 源码(`/home/t00842504/cann-repo/runtime/`)完整调用链:

```
aclrtMemImportFromShareableHandle  (ACL API, memory.cpp:1243)
  → rtMemImportFromShareableHandle  (runtime C API, api_c.cc:3744)
    → ApiImpl::ImportFromShareableHandle  (★ api_impl.cc:7323)
        ├─ GetPhyDevIdByMemShareHandle → 取 peer 物理设备 ID
        ├─ dev->EnableP2PWithOtherDevice(peerPhyDeviceId)  ← ★ P2P enable
        └─ NpuDriver::ImportFromShareableHandle → HAL vmm_import(纯 VA 映射)
```

`ApiImpl::ImportFromShareableHandle`(api_impl.cc:7323)在调 HAL import **之前**,
先调 `EnableP2PWithOtherDevice`(raw_device.cc:2198):
```cpp
if (phyDeviceId != peerPhyDeviceId) {              // die 对同物理芯片: phy==peer, 跳过
    EnableP2P(peerDeviceId, phyDeviceId, 0U);      // 方向 1
    EnableP2P(deviceId_, peerPhyDeviceId, 0U);     // 方向 2
}
```
→ `NpuDriver::EnableP2P`(npu_driver.cc:1011)→ **`halDeviceEnableP2P`**。

HAL 层 `vmm_import`(svm_vmm.c:1990)本身**不调 P2P**,只做 dir_check + 取 src_va +
建 handle;P2P enable 是它**上层 runtime** 干的。

| 实验现象 | 源码解释 |
|---|---|
| 跨卡 import 消耗 P2P 槽位 | `phy != peer` → 调 `EnableP2P` **两次(双向)** → `halDeviceEnableP2P` |
| DisablePeerAccess ok 数 == 跨卡 import 成功数 | 每次跨卡 import 都调了 `EnableP2P`,创建了可被 Disable 拆除的状态 |
| die 对 ctx 技巧 import 的 Disable = 507899 | `phyDeviceId == peerPhyDeviceId`(同物理芯片)→ **跳过** EnableP2P |
| 失败矩阵与 IPC PEER flag 版相同 | 两条路径最终都走 `NpuDriver::EnableP2P` → `halDeviceEnableP2P` |
| "一条 import = 双向关系, 两端各扣 1 槽" | runtime 一次 import 调 `EnableP2P` **两次**(两方向), 正好是双向 |

runtime 在一次 import 里调两次 `EnableP2P`(两个方向),正是驱动注释"Both directions
must be set to take effect"的 runtime 侧实现——**runtime 代用户完成双向 enable**,所以
用户只需一次 import 就能得到完整的双向 P2P 关系。这与实验数据推出的"一条 import = 一条
双向关系"模型完全一致。

### 其他有效观测

- **VA 矩阵 CONSISTENT**:gva_pool 槽位窗口构造(每 rank Reserve 128MB,
  slot p = base + p*16MB)下 8 进程 VA 完全一致(base 均 `0x12c140000000`),
  且这是**按构造保证**的一致,不是 IPC 的顺序性巧合——若需要全网同 VA,
  VMM 槽位窗口是可行方案。
- 建池阶段 8 rank 并发 MallocPhysical/Reserve/Map/Export 无任何失败
  (首轮 r0 export 偶发失败为环境噪声,复跑消失)。

### 最终结论

**IPC 与 VMM 两条路径在 310P Duo 上撞同一堵墙:8-rank 全连接(56 条有向链路)
受每卡 8 个 P2P 槽位硬约束,不可达。** 注意配额只约束**跨卡**关系——die 对
(同卡两 die)import 不消耗槽位(runtime 判 phy 相同跳过 EnableP2P),但这也
救不了全连接:即使 die 对全免费,每卡仍需 12 条跨卡关系 > 8。
"换 VMM 绕过配额"的假设被否定
(VMM import 隐式消耗同样配额)。该限制有驱动代码级实证:HAL 注释
"Ascend310P Duo 两 die share 8 MAX enable number"(ascend_hal_base.h:1695)+
TX ATU 窗口数 16/每连接 2 = 8(devdrv_atu.h),强制执行点在闭源固件
(DEVDRV_CFG_TX_ATU)。可行的全连接替代仍是:
1. 每卡 1 rank 的 4-rank 全连接(07 已验证 IPC 版可行,VMM 同理);
2. 环/倍增等稀疏拓扑(hcomm 模式,每卡 ≤4 条入向,远低于配额);
3. 若需要全网同 VA:VMM + 槽位窗口构造(本测试已验证 VA CONSISTENT)。
