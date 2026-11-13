# 04：跨卡VMM分配、共享与映射

使用ACL物理内存、共享句柄、地址预留和映射建立跨进程设备池。验证异步D2D与Host读写，并对比导入时的设备上下文。

## 测试步骤与API调用顺序

rank0为导出方A，rank1为导入方B；共享句柄通过MPI传递。物理内存句柄、预留虚拟地址和映射分别管理。

| 阶段 | 导出方A | 导入方B |
|---|---|---|
| 分配与导出 | 启用到B的peer；查询粒度并`aclrtMallocPhysical`；`aclrtReserveMemAddress` → `aclrtMapMem` → `aclrtMemSetAccess`；H2D写入；`aclrtMemExportToShareableHandle` | 接收共享句柄 |
| step16对照 | 此前已启用到B的peer，另行导出对照句柄 | 尚未显式启用到A的peer，直接`aclrtMemImportFromShareableHandle`，记录返回值并释放句柄 |
| 主数据路径 | 等待B完成后D2H检查反向写入 | 启用到A的peer；在本地设备上下文导入；预留、映射、设置权限；异步D2D读取并回读，再反向写入 |
| 上下文对照 | 导出另一块独立物理内存 | `aclrtSetDevice(A)` → `aclrtMemImportFromShareableHandle(sh, B, ...)` → `aclrtSetDevice(B)`；仅检查导入及释放 |
| Host与性能 | 复核B直接写入的前16MiB，最终复核完整主块 | 导入地址上D2H/H2D；主块高16MiB用于读带宽，低16MiB用于写带宽 |

主物理块32MiB，独立对照块8/16MiB。分配辅助函数依次尝试多个memAttr，并按所选属性的粒度分配；命中一个可用属性就停止，不能把回退命中解释为其他未执行属性均不支持。

导入方先`aclrtUnmapMem` → `aclrtFreePhysical` → `aclrtReleaseMemAddress`，MPI同步后导出方释放自己的映射、物理句柄和地址。随后拆除peer状态、销毁stream并重置设备。导入句柄与原物理内存的所有权应分开理解。

step16只控制导入方是否显式启用peer，不是“双侧均未启用”的实验。该区别是保留本项步骤说明的必要条件。

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/04_vmm_cross
A_DEV=2 B_DEV=6 bash run.sh
```

## 2026-11-13复测

10006，Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。设备及数据规模见下表。

| 检查项 | 本轮结果 |
|---|---|
| 设备与大小 | 2、6；主要物理块32MiB，对照块8/16MiB |
| 本地分配、导出、导入与映射 | 成功 |
| 异步D2D及Host直接双向读写 | 校验通过 |
| 省略导入方EnablePeerAccess | 导入成功；此前导出方已调用EnablePeerAccess |
| 先切到导出方上下文再导入 | 导入成功；该项只观察导入，不另建数据窗口 |
| 带宽 | 16MiB，3次预热、30次异步提交后同步：读12.96、写23.31 GB/s |
| 总结果 | PASS，两个rank退出0 |

跨卡VMM共享数据路径可用。关于不显式启用peer的结论，应以双方调用顺序为条件；本用例不构成双方都未启用peer的独立验证，完整对照见15项。

## 历史memAttr探针结果

本目录另有`test_memattr.c`。以下为旧README在2026-08-25、CANN 9.1.0的独立属性探针记录，不是本轮主程序的回退链覆盖情况。本轮未重跑此探针，参数、返回码和分配粒度供后续逐项复测。

| memAttr | 旧记录结果 | 成功时粒度 |
|---|---|---|
| ACL_MEM_NORMAL | 成功 | 2MiB |
| ACL_MEM_HUGE | 成功 | 2MiB |
| ACL_MEM_P2P_NORMAL | 成功 | 2MiB |
| ACL_MEM_P2P_HUGE | 成功 | 2MiB |
| ACL_HBM_MEM_HUGE | 成功 | 2MiB |
| ACL_HBM_MEM_NORMAL | 成功 | 2MiB |
| ACL_MEM_HUGE1G、ACL_MEM_P2P_HUGE1G | 207000 | — |
| ACL_HBM_MEM_P2P_HUGE、ACL_HBM_MEM_P2P_NORMAL | 100000 | — |
| ACL_DDR_MEM_HUGE、ACL_DDR_MEM_NORMAL | 100000 | — |
| ACL_DDR_MEM_P2P_HUGE、ACL_DDR_MEM_P2P_NORMAL | 100000 | — |

主程序依次尝试属性，命中后停止；独立探针逐项调用，覆盖范围不同。不应根据前者“第一个成功属性”得出后续所有属性均不支持。



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

**与官方文档有差异的地方**: `ACL_HBM_MEM_HUGE`/`ACL_HBM_MEM_NORMAL` 文档标注仅 350/A3/A2，但 310P3 实测可用。


## 核心结论

### 跨卡 VMM Import 行为(310P3 确认)

| 项目 | 结论 |
|------|------|
| 本进程 ctx import | EnablePeerAccess **非必须**,不做也成功 |
| ctx 技巧(SetDevice 切换) | 同卡 die 对方案,跨卡也能用但非必须 |
| async D2D 读写 | 双向校验 OK |
| host 直达 | D2H/H2D 均 OK |

### 带宽

- D2D 读: ~11.1 GB/s
- D2D 写: ~13.1 GB/s
- 方向不对称(写比读快约 19%)

## 测试步骤

| 步骤 | 内容 | 结果 |
|------|------|------|
| step1 | Exporter 分配 32MB 物理内存(MEM_P2P_HUGE)，Export 为 sh1 | OK |
| step16 | **对照**: Importer 不做 EnablePeerAccess 直接 Import | 成功(310P3 确认) |
| step2 | Importer Import sh1，双向 async D2D 读写 + pattern 校验 | OK |
| step3 | **对照**: ctx 技巧(同卡 die 对方案)也能跨卡用 | 成功(310P3 确认) |
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
│    → 310P3 实测: 不调用也能 Import,此处仅为冗余保留                         │
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
│    → 310P3 实测: 不调用也能 Import,此处仅为冗余保留                         │
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
│ 目的: 验证不做 EnablePeerAccess 直接 Import 是否可行                         │
│                                                                             │
│ 步骤:                                                                       │
│ 1. Exporter 分配独立 8MB 物理内存,Export 为 sh_p2p,发给 Importer            │
│ 2. Importer 不做 EnablePeerAccess,直接调用 Import(sh_p2p, devB, &h)         │
│ 3. 结果 ret=0(成功)                                                         │
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
│ 4. 结果 ret=0(成功)                                                         │
│ 5. Importer 调用 aclrtSetDevice(devB) 切回本进程 ctx                        │
│                                                                             │
│ 结论: ctx 技巧是同卡 die 对方案,跨卡也能用但非必须                           │
└─────────────────────────────────────────────────────────────────────────────┘
```
