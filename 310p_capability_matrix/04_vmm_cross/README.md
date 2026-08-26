# 简单版本——04_vmm_cross — VMM shareable 链路跨卡(PCIe)

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64  
MPI 双进程: rank0=exporter(dev A, `A_DEV` 默认 0), rank1=importer(dev B, `B_DEV` 默认 2)

## 310P3 内存属性支持

**310P3 属于 Atlas 推理系列(非 A3/A2)**。实测支持 6 种 memAttr 类型:

| 类型 | 说明 | 官方文档 | 310P3 实测 | gran |
|------|------|----------|------------|------|
| `ACL_MEM_NORMAL` | 普通内存 | 通用 | ✓ 成功 | 2MB |
| `ACL_MEM_HUGE` | 2M 大页 | 通用 | ✓ 成功 | 2MB |
| `ACL_MEM_P2P_NORMAL` | P2P 普通 | 通用 | ✓ 成功 | 2MB |
| `ACL_MEM_P2P_HUGE` | P2P 2M 大页 | 通用 | ✓ 成功 | 2MB |
| `ACL_HBM_MEM_HUGE` | HBM 2M 大页 | ✗ 仅 350/A3/A2 | **✓ 成功** ⚠ | 2MB |
| `ACL_HBM_MEM_NORMAL` | HBM 普通 | ✗ 仅 350/A3/A2 | **✓ 成功** ⚠ | 2MB |

**不支持的类型**:
- 1G 大页(`ACL_MEM_HUGE1G`/`ACL_MEM_P2P_HUGE1G`): ret=207000
- HBM_P2P 组合: ret=100000
- DDR 前缀(仅 Host 内存): ret=100000

**⚠ 与文档不符**: `ACL_HBM_MEM_HUGE`/`ACL_HBM_MEM_NORMAL` 文档标注仅 350/A3/A2，但 310P3 实测可用。

## 核心结论

### 跨卡 VMM Import 行为(310P3 比预期宽松)

| 项目 | 预期 | 实测 | 结论 |
|------|------|------|------|
| 本进程 ctx import | 需双向 EnablePeerAccess | 不做 EnablePeerAccess 也成功 | **非必须** |
| ctx 技巧(SetDevice 切换) | 仅同卡 die 对有效 | 跨卡也成功 | **跨卡可用** |
| async D2D 读写 | - | 双向校验 OK | **正常** |
| host 直达 | - | D2H/H2D 均 OK | **正常** |

### 带宽

- D2D 读: ~11.7 GB/s
- D2D 写: ~14.1 GB/s
- 方向不对称(写比读快约 21%)

## 测试步骤

| 步骤 | 内容 | 结果 |
|------|------|------|
| step1 | Exporter 分配 32MB 物理内存(MEM_P2P_HUGE)，Export 为 sh1 | OK |
| step16 | **对照**: Importer 不做 EnablePeerAccess 直接 Import | 意外成功 |
| step2 | Importer Import sh1，双向 async D2D 读写 + pattern 校验 | OK |
| step3 | **对照**: ctx 技巧 Import | 意外成功 |
| step5 | host 直达 D2H/H2D 16MB | OK |
| step4 | 带宽测试(3 预热 + 30×16MB async) | OK |

## API 调用链

**Exporter (dev0)**:
```
MallocPhysical(32MB) → ReserveMemAddress → MapMem → SetAccess(RW)
→ EnablePeerAccess(dev2) → H2D pattern → ExportToShareableHandle → sh1
```

**Importer (dev2)**:
```
EnablePeerAccess(dev0) → ImportFromShareableHandle(sh1, dev2)
→ ReserveMemAddress → MapMem → SetAccess(RW) → D2D/host 访问
```

**Cleanup**: Importer 先释放 → MPI_Barrier → Exporter 后释放 → DisablePeerAccess → ResetDevice

## 运行

```bash
./run.sh                    # 默认 dev0→dev2
A_DEV=1 B_DEV=4 ./run.sh    # 其他设备组合
```

## D 态挂死修复

**问题**: 旧版退出时残留 D 状态进程(SIGKILL 不可回收)  
**根因**: EnablePeerAccess 建立的 P2P 状态 + mmu notifier 释放 = 驱动 SRCU 死锁  
**修复**: cleanup 在 ResetDevice 前调用 `aclrtDeviceDisablePeerAccess(peer)`  
**验证**: exit=0，无 D 态残留

## 310P3 所属系列与内存属性支持

**310P3 属于 Atlas 推理系列(非 A3/A2 系列)**。根据 CANN 官方文档,`aclrtPhysicalMemProp.memAttr`
支持的类型及 310P3 实测结果(`test_memattr` 验证):

| 类型 | 说明 | 官方文档支持范围 | 310P3 实测 | gran(B) | 备注 |
|------|------|------------------|------------|---------|------|
| `ACL_MEM_NORMAL` | 普通内存 | 通用 | ✓ 成功 | 2MB | |
| `ACL_MEM_HUGE` | 2M 大页 | 通用 | ✓ 成功 | 2MB | |
| `ACL_MEM_HUGE1G` | 1G 大页 | ✗ 仅 A3/A2 | ✗ ret=207000 | - | 207000 ≠ 100000,驱动识别为"不支持" |
| `ACL_MEM_P2P_NORMAL` | P2P 普通 | 通用 | ✓ 成功 | 2MB | |
| `ACL_MEM_P2P_HUGE` | P2P 2M 大页 | 通用 | ✓ 成功 | 2MB | |
| `ACL_MEM_P2P_HUGE1G` | P2P 1G 大页 | ✗ 仅 A3 | ✗ ret=207000 | - | |
| `ACL_HBM_MEM_HUGE` | HBM 2M 大页 | ✗ 仅 350/A3/A2 | **✓ 成功** | 2MB | **⚠ 与文档不符!310P3 实际可用** |
| `ACL_HBM_MEM_NORMAL` | HBM 普通 | ✗ 仅 350/A3/A2 | **✓ 成功** | 2MB | **⚠ 与文档不符!310P3 实际可用** |
| `ACL_HBM_MEM_P2P_HUGE` | HBM P2P 2M | ✗ 仅 A3/A2 | ✗ ret=100000 | - | |
| `ACL_HBM_MEM_P2P_NORMAL` | HBM P2P 普通 | ✗ 仅 A3/A2 | ✗ ret=100000 | - | |
| `ACL_DDR_MEM_HUGE` | DDR 大页 | 仅 Host | ✗ ret=100000 | - | |
| `ACL_DDR_MEM_NORMAL` | DDR 普通 | 仅 Host | ✗ ret=100000 | - | |
| `ACL_DDR_MEM_P2P_HUGE` | DDR P2P 2M | 仅 Host | ✗ ret=100000 | - | |
| `ACL_DDR_MEM_P2P_NORMAL` | DDR P2P 普通 | 仅 Host | ✗ ret=100000 | - | |

**实测结论**:310P3 支持 **6 种** memAttr 类型:
- 全部无介质前缀非 1G 类型:`ACL_MEM_NORMAL`、`ACL_MEM_HUGE`、`ACL_MEM_P2P_NORMAL`、`ACL_MEM_P2P_HUGE`
- HBM 非 P2P 类型:`ACL_HBM_MEM_HUGE`、`ACL_HBM_MEM_NORMAL`(**与文档不符,文档说仅 350/A3/A2**)
- 所有 1G 大页类型返回 207000(驱动识别为不支持,但错误码不同于 100000)
- 所有 DDR 前缀、HBM_P2P 组合返回 100000

**关键区别**:
- **无介质前缀**(`ACL_MEM_*`):通用类型,除 1G 大页外均支持
- **HBM 前缀**(`ACL_HBM_MEM_*`):文档说仅 350/A3/A2,但 310P3 实测非 P2P 的两种可用
- **DDR 前缀**(`ACL_DDR_MEM_*`):仅 Host 内存,310P3 是 Device 卡,全部不支持
- **P2P 组合**:`HBM_MEM_P2P_*` 不支持,仅 `ACL_MEM_P2P_*` 可用于跨设备复制

**对 test.c 七级回退链的影响**:
当前回退链中 `ACL_HBM_MEM_P2P_HUGE` 等返回 100000,而 `ACL_MEM_NORMAL`、`ACL_MEM_HUGE`、
`ACL_MEM_P2P_NORMAL` 实际可用但未在链中。当前链第 5 级 `ACL_MEM_P2P_HUGE` 已能命中,
功能不受影响,但回退链可以扩展为 10 级以覆盖更多可用类型。

## API 调用链详解

### Exporter 侧(dev0)—— 分配物理内存并导出

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. aclrtMallocPhysical(32MB, prop)                                          │
│    → 分配 32MB 物理内存(handle=h_own)                                       │
│    → prop.memAttr 七级回退: HBM_P2P_HUGE → ... → MEM_P2P_HUGE → ...        │
│    → 310P3 实测仅 MEM_P2P_HUGE 成功,granularity=2MB                         │
├─────────────────────────────────────────────────────────────────────────────┤
│ 2. aclrtReserveMemAddress(32MB, hint=NULL)                                  │
│    → 在虚拟地址空间预留 32MB 区间(返回 VA=ownVA)                            │
│    → hint=NULL 表示由驱动选择地址                                           │
├─────────────────────────────────────────────────────────────────────────────┤
│ 3. aclrtMapMem(ownVA, 32MB, h_own)                                          │
│    → 将物理内存映射到预留的虚拟地址                                          │
│    → 映射后 ownVA 可读写                                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│ 4. aclrtMemSetAccess(ownVA, 32MB, RW, devA)                                 │
│    → 设置 dev0 对 ownVA 有读写权限                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│ 5. aclrtDeviceEnablePeerAccess(devB, 0)                                     │
│    → 开启 dev0→dev2 的 P2P 访问(双向 P2P 之 exporter 侧)                   │
│    → 原预期是 Export 前必须调用,但 step16 实测 310P3 上不调用也能 Import     │
├─────────────────────────────────────────────────────────────────────────────┤
│ 6. aclrtMemcpy(ownVA, hbuf, H2D)                                            │
│    → 将 pattern 数据从 host 写入 device(ownVA)                              │
├─────────────────────────────────────────────────────────────────────────────┤
│ 7. aclrtMemExportToShareableHandle(h_own, DISABLE_PID, &sh1)                │
│    → 将物理内存 handle 导出为 shareable handle(u64)                         │
│    → DISABLE_PID 表示不校验进程 ID,允许跨进程访问                           │
│    → sh1 经 MPI 发送给 Importer                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Importer 侧(dev2)—— 导入并访问跨卡内存

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. aclrtDeviceEnablePeerAccess(devA, 0)                                     │
│    → 开启 dev2→dev0 的 P2P 访问(双向 P2P 之 importer 侧)                   │
│    → 原预期是 Import 前必须调用,但 step16 实测 310P3 上不调用也能 Import     │
├─────────────────────────────────────────────────────────────────────────────┤
│ 2. aclrtMemImportFromShareableHandle(sh1, devB, &h_imp)                     │
│    → 将 exporter 发来的 sh1 导入为本进程的物理内存 handle(h_imp)            │
│    → devB=dev2 表示在本进程当前设备(dev2)上导入                             │
├─────────────────────────────────────────────────────────────────────────────┤
│ 3. aclrtReserveMemAddress(32MB, hint=NULL)                                  │
│    → 在 importer 虚拟地址空间预留 32MB 区间(返回 VA=vmmVA)                  │
├─────────────────────────────────────────────────────────────────────────────┤
│ 4. aclrtMapMem(vmmVA, 32MB, h_imp)                                          │
│    → 将导入的物理内存映射到预留的虚拟地址                                    │
│    → 映射后 vmmVA 可读写(实际数据在 dev0 的物理内存上)                      │
├─────────────────────────────────────────────────────────────────────────────┤
│ 5. aclrtMemSetAccess(vmmVA, 32MB, RW, devB)                                 │
│    → 设置 dev2 对 vmmVA 有读写权限                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│ 6. 数据访问                                                                 │
│    → async D2D: vmmVA ↔ local(本地缓冲)                                    │
│    → host 直达: vmmVA ↔ hbuf(host 内存)                                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 对照实验(step16)—— EnablePeerAccess 必要性测试

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 目的: 验证不做 EnablePeerAccess 直接 Import 是否会失败                       │
│                                                                             │
│ 步骤:                                                                       │
│ 1. Exporter 分配独立 8MB 物理内存,Export 为 sh_p2p,发给 Importer            │
│ 2. Importer 不做 EnablePeerAccess,直接调用 Import(sh_p2p, devB, &h)         │
│ 3. 原预期 ret=507899(失败),实测 ret=0(成功)                                 │
│ 4. 释放资源(不建立完整的 Map/Access 窗口)                                   │
│                                                                             │
│ 结论: 310P3 上 EnablePeerAccess 不是跨卡 Import 的必要条件                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 对照实验(step3)—— ctx 技巧测试

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 目的: 验证 "SetDevice(A) 后 Import(sh, B)" 是否跨卡可用                     │
│                                                                             │
│ 步骤:                                                                       │
│ 1. Exporter 分配独立 16MB 物理内存,Export 为 sh2,发给 Importer              │
│ 2. Importer 当前 ctx=devB,调用 aclrtSetDevice(devA) 切换到 devA 的 ctx      │
│ 3. Importer 调用 Import(sh2, devB, &h2) —— 用 devA 的 ctx 导入到 devB      │
│ 4. 原预期 ret=507899(失败),实测 ret=0(成功)                                 │
│ 5. Importer 调用 aclrtSetDevice(devB) 切回本进程 ctx                        │
│                                                                             │
│ 结论: ctx 技巧在 310P3 跨卡场景也可用,不仅限于同卡 die 对                   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Cleanup 顺序(铁律)

```
Importer 先释放:
  UnmapMem(vmmVA) → FreePhysical(h_imp) → ReleaseMemAddress(vmmVA)
  ↓
MPI_Barrier(等待 Importer 全部释放)
  ↓
Exporter 后释放:
  UnmapMem(ownVA/ctrlVA/p2p_ctrlVA) → FreePhysical(h_own/h_ctrl/h_p2p_ctrl) → ReleaseMemAddress(...)
  ↓
双方 DisablePeerAccess → ResetDevice → Finalize
```

## ★实测结论(2026-08-25)

### 各步骤一句话总结

| 步骤 | 做了什么 | 结论 |
|------|----------|------|
| step1 | Exporter 在 dev0 上分配 32MB 物理内存(七级 memAttr 回退,仅 `MEM_P2P_HUGE` 成功),Export 为 shareable handle sh1=0x3 发给 Importer | OK,跨卡 Export 链路通 |
| step16 | **对照实验**:Exporter 分配独立 8MB 物理内存 Export 为 sh_p2p;Importer **不做 EnablePeerAccess** 直接尝试 Import | **意外成功**(ret=0),310P3 上 EnablePeerAccess 不是跨卡 Import 的必要条件 |
| step2 | Importer 在 dev2 上用本进程 ctx Import sh1,双向 async D2D 读写 32MB 并逐字校验 pattern | OK,跨卡 VMM 双向读写正确 |
| step3 | 对照实验:Importer 用 ctx 技巧(SetDevice(A) 后 Import sh2 到 B),验证跨卡是否可行 | **意外成功**(ret=0),推翻了"ctx 技巧仅 die 对有效"的假设 |
| step5 | Importer 对 imported VA 直接做 host D2H 读 / H2D 写各 16MB,Exporter 复核 | OK,host 直达跨卡可用 |
| step4 | 带宽测试:3 次预热 + 30×16MB async 提交 + 1 次 Sync | 读 11.66 GB/s,写 14.14 GB/s |
| 终验 | Exporter D2H 校验全 32MB 是否为 TAG_BACK pattern | OK,数据完整 |