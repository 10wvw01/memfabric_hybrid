# 10_vmm_hardcell — VMM 跨卡 import 验证

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 1),rank1=importer(dev B,`B_DEV` 默认 4)。

## 验证能力
- VMM 跨卡 import 可达性:EnablePeerAccess(双向)ret=0 ✓(能打通 P2P 通路)
- 方式一(本进程 ctx):`aclrtMemImportFromShareableHandle(sh1, B, &h)`(ctx=B)
  → 验证 import 是否成功
- 方式二(ctx 技巧):`aclrtSetDevice(A)` → `ImportFromShareableHandle(sh2, B, &h2)`
  (ctx=A=导出方,devId=B)→ 验证 import 是否成功
- 方式三(替代方式,不用 SetDevice):`aclrtCreateContext(&ctx, A)` +
  `aclrtSetCurrentContext(ctx)` → `ImportFromShareableHandle(sh3, B, &h3)` →
  `SetDevice(B)` 切回 + `aclrtDestroyContext(ctx)`。与方式二等效(ctx 落在导出方
  →跳过自动 P2P),但 ctx 生命周期显式可控,不 lazy 创建 A 的 primary ctx
- 三种方式均成功即 PASS;若失败记录错误码

> **ctx 技巧的适用场景(源码+实测验证, CANN 9.1.0)**:方式二仅在**同卡跨 die**(如
> dev0↔dev1,两 die 共享一个 PCIe 设备)场景有意义。机制:import 内部
> (`api_impl.cc:7338`)用**当前 ctx 的设备**调用 `EnableP2PWithOtherDevice`;同卡
> die 对的 P2P 被内核显式拒绝(`devdrv_ctrl.c:4424` 注释 `/* 2DIe : 1PCIe 2PF */`,
> 返回 -EINVAL→507899),导致方式一 import 在自动 P2P 步骤夭折。ctx 技巧使当前
> ctx 设备==导出方设备,`raw_device.cc:2206` 直接**跳过 P2P 启用**,import ioctl
> 本身对同卡跨 die 无限制,故成功。跨卡场景 P2P 能打通,方式一即可,方式二冗余。
> 实测:跨卡 dev1→dev4 两方式均 OK;同卡 dev0→dev1 方式一 FAIL 507899、方式二 OK。
> 替代方式 `aclrtCreateContext(A)`+`aclrtSetCurrentContext`(实测同效,推荐:
> context 生命周期显式可控)。

## API 链
- exporter(dev A):`aclrtDeviceEnablePeerAccess(B, 0)` → `aclrtMallocPhysical` 16MB
  (memAttr 七级回退链 `ACL_HBM_MEM_P2P_HUGE` → `ACL_HBM_MEM_P2P_NORMAL` →
  `ACL_DDR_MEM_P2P_HUGE` → `ACL_DDR_MEM_P2P_NORMAL` → `ACL_MEM_P2P_HUGE` →
  `ACL_MEM_P2P_NORMAL` → `ACL_HBM_MEM_HUGE`;310P3 实测仅 `ACL_MEM_P2P_HUGE`
  成功;16MB 按 granularity=2MB 对齐,MapMem size == MallocPhysical size)→
  `aclrtReserveMemAddress(NULL hint)` → `aclrtMapMem` →
  `aclrtMemSetAccess(RW, A)` → H2D pattern →
  `aclrtMemExportToShareableHandle(ACL_MEM_HANDLE_TYPE_NONE,
  ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION)` → sh1(u64)经 MPI 发给 importer
- importer(dev B)方式一(本进程 ctx):`aclrtDeviceEnablePeerAccess(A, 0)`
  → `aclrtMemImportFromShareableHandle(sh1, B_dev, &h)`(本进程 ctx = dev B)
  → 成功则 `aclrtFreePhysical(h)` 回收(仅观测 import,不建窗口)
- importer 方式二(ctx 技巧):exporter 第二块独立 16MB 物理内存 export sh2(规避同
  sh 二次 import 干扰);importer `aclrtSetDevice(A)`(借导出方 ctx)→
  `ImportFromShareableHandle(sh2, B_dev, &h2)`(ctx=A,devId=B)→ 成功则
  `aclrtFreePhysical(h2)` 回收;测完 `aclrtSetDevice(B)` 切回本进程 ctx
- importer 方式三(替代方式):exporter 第三块独立 16MB 物理内存 export sh3;
  importer `aclrtCreateContext(&expCtx, A)`(显式建导出方 ctx,不依赖 SetDevice)→
  `aclrtSetCurrentContext(expCtx)`(仅写线程局部变量,零驱动交互)→
  `ImportFromShareableHandle(sh3, B_dev, &h3)` → 成功则 `aclrtFreePhysical(h3)` 回收 →
  `aclrtSetDevice(B)` 切回 primary ctx → `aclrtDestroyContext(expCtx)`
- cleanup 顺序(UnmapMem → FreePhysical → ReleaseMemAddress):importer 无 imported VA
  (仅观测 import);exporter barrier 后同序释放 16MB 主块 + 16MB 对照块 + 16MB 方式三块

校验 pattern:`(rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 0x2(sh1)/0x5(sh2)/0x7(sh3)
每步不同,防残留数据假阳性(本测试虽不回读,仍写入以保持链路真实性与链上测试一致)。

## 运行方法
```
./run.sh                    # 默认 dev1→dev4(跨卡)
A_DEV=0 B_DEV=1 ./run.sh    # 同卡跨 die(dev0/dev1 共享 PCIe 0000:18:00.0)
A_DEV=2 B_DEV=3 ./run.sh    # 同卡跨 die(第二张卡)
A_DEV=1 B_DEV=0 ./run.sh    # 同卡跨 die 反向
A_DEV=4 B_DEV=1 ./run.sh    # 跨卡反向
```

## 预期结果
- **跨卡对**(如 dev1→dev4):EnablePeerAccess ret=0;方式一/二/三全部 ret=0 成功
- **同卡跨 die 对**(如 dev0→dev1):EnablePeerAccess FAIL 507899;方式一 FAIL 507899
  (死于内核同芯片 P2P 检查);方式二/三成功(跳过 P2P,import ioctl 无同卡限制)
- `[10] RESULT: PASS`:exporter 正向 1/3/5 全 OK + 三种 import 方式都有结果即 PASS

## 实测结论(2026-08-27,CANN 9.1.0)

### 三种 import 方式定义

三种方式调用**完全相同**的 `aclrtMemImportFromShareableHandle(sh, B, &h)`(devId
参数都传 devB),唯一区别是**调用瞬间当前线程 context 绑定在哪个设备**——这决定
runtime 内部是否在"当前 ctx 设备 ↔ 导出方设备"之间自动启用 P2P:

| 方式 | 调用 import 时的当前 ctx | ctx 如何而来 | 本质 |
|------|--------------------------|--------------|------|
| 方式一(本进程 ctx) | devB(importer 自己的设备) | 初始化时 `aclrtSetDevice(B)` 的 primary ctx | 不做任何切换,基线行为 |
| 方式二(ctx 技巧) | devA(导出方设备) | `aclrtSetDevice(A)` 借导出方 primary ctx | ctx 落在导出方 → runtime 跳过自动 P2P |
| 方式三(CreateCtx 替代) | devA(导出方设备) | `aclrtCreateContext(A)` + `aclrtSetCurrentContext` | 与方式二同效,但 ctx 显式创建/销毁,不 lazy 建 primary ctx |

### 实测矩阵

| 场景 | EnablePeerAccess | 方式一(本进程ctx) | 方式二(ctx技巧) | 方式三(CreateCtx替代) |
|------|------------------|-------------------|-----------------|----------------------|
| 跨卡 dev1→dev4 | ret=0 ✓ | ret=0 成功 | ret=0 成功 | ret=0 成功 |
| 跨卡 dev4→dev1 | ret=0 ✓ | ret=0 成功 | ret=0 成功 | ret=0 成功 |
| 同卡跨die dev0→dev1 | FAIL 507899 | FAIL 507899 | ret=0 成功 | ret=0 成功 |
| 同卡跨die dev1→dev0 | FAIL 507899 | FAIL 507899 | ret=0 成功 | ret=0 成功 |
| 同卡跨die dev2→dev3 | FAIL 507899 | FAIL 507899 | ret=0 成功 | ret=0 成功 |

读法:**跨卡**三方式行为一致(P2P 能打通,无需技巧);**同卡跨 die**只有方式一失败,
方式二/三靠"ctx 落在导出方"绕过 P2P 检查。

**核心规律**(源码+实测双重验证):
- 同卡跨 die 时方式一与 EnablePeerAccess 返回**同一个错误码 507899**——两者死在
  同一个内核检查(`devdrv_p2p_para_check`,devdrv_ctrl.c:4482/:4424)
- 方式二/三成功的原因:import 内部用**当前 ctx 的设备**调用
  `EnableP2PWithOtherDevice`(api_impl.cc:7338);当前 ctx=导出方设备时
  `phyId == peerPhyId` 直接跳过 P2P 启用(raw_device.cc:2206),import ioctl
  本身对同卡跨 die 无任何限制(svm_master_mem_share.c:1135)
- 方式三(替代方式)与方式二等效,且 ctx 生命周期显式可控(Create/Destroy),
  不 lazy 创建 primary ctx,已作为正式测试用例(step 4)常驻本测试

RESULT: PASS。Cleanup 全部正常(含方式三 DestroyContext ret=0),无 D 态挂死。

### D 态挂死防护(沿用 03/04 修复)
本测试做跨卡 `aclrtDeviceEnablePeerAccess`,进程退出时若不显式拆除 P2P 状态会触发
驱动 SRCU 死锁(exit_mmap → synchronize_srcu 永久等待,SIGKILL 不可回收,耗尽
hugepages)。cleanup 在 `aclrtResetDevice` 前加 `aclrtDeviceDisablePeerAccess(peer)`
对称拆除(r0 拆 devB、r1 拆 devA);`run.sh` 加 `timeout 60` 兜底。详见
03_ipc_cross/README.md 的三条证据链。运行后检查 `ps aux|grep test` 不应有 D 态残留。
