# 10_vmm_hardcell — VMM 跨卡边界探测(原 ✗ 单元格假设已推翻)

Atlas 300I Duo (310P3, 4 卡 × 2 die = 8 dev) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 1),rank1=importer(dev B,`B_DEV` 默认 4)。

> **原为负向测试**:验证 VMM 的不可达边界。原假设 ✗ 单元格 = 跨卡但 VMM import 不可达的
> 有向对(如 dev1→dev4)。但实测(CANN 9.1.0)显示 dev1→dev4 VMM import **可达**,
> 推翻了原假设。
> **IPC-PEER 链路一直可用(见 03_ipc_cross),VMM 在当前版本也已打通。**

## 验证能力
- VMM ✗ 单元格边界:EnablePeerAccess(双向)ret=0 ✓(obs,能打通 P2P 通路)
- 方式一(本进程 ctx):`aclrtMemImportFromShareableHandle(sh1, B, &h)`(ctx=B)
  → **预期 507899**(EXPECTED-FAIL)
- 方式二(ctx 技巧):`aclrtSetDevice(A)` → `ImportFromShareableHandle(sh2, B, &h2)`
  (ctx=A=导出方,devId=B)→ **预期 507899**(EXPECTED-FAIL)
- 两种 import 方式都失败即文档化 VMM 的不可达边界;无 D2D / 无带宽 / 无 host 直达
  (import 都失败,无 imported VA 可用)

## API 链
- exporter(dev A):`aclrtDeviceEnablePeerAccess(B, 0)`(双向 P2P 之 exporter 侧,
  ✗ 单元格预期 ret=0)→ `aclrtMallocPhysical` 16MB(memAttr 七级回退链
  `ACL_HBM_MEM_P2P_HUGE` → `ACL_HBM_MEM_P2P_NORMAL` → `ACL_DDR_MEM_P2P_HUGE` →
  `ACL_DDR_MEM_P2P_NORMAL` → `ACL_MEM_P2P_HUGE` → `ACL_MEM_P2P_NORMAL` →
  `ACL_HBM_MEM_HUGE`;310P3 实测仅 `ACL_MEM_P2P_HUGE` 成功;16MB 按 granularity=2MB
  对齐,MapMem size == MallocPhysical size)→ `aclrtReserveMemAddress(NULL hint)`
  → `aclrtMapMem` → `aclrtMemSetAccess(RW, A)` → H2D pattern →
  `aclrtMemExportToShareableHandle(ACL_MEM_HANDLE_TYPE_NONE,
  ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION)` → sh1(u64)经 MPI 发给 importer
- importer(dev B)方式一(本进程 ctx):`aclrtDeviceEnablePeerAccess(A, 0)`(预期 0)
  → `aclrtMemImportFromShareableHandle(sh1, B_dev, &h)`(**本进程 ctx** = dev B)
  → 预期 507899;若意外成功则 `aclrtFreePhysical(h)` 回收(仅观测 import,不建窗口)
- importer 方式二(ctx 技巧):exporter 第二块独立 16MB 物理内存 export sh2(规避同
  sh 二次 import 干扰);importer `aclrtSetDevice(A)`(借导出方 ctx)→
  `ImportFromShareableHandle(sh2, B_dev, &h2)`(ctx=A,devId=B)→ 预期 507899;
  测完 `aclrtSetDevice(B)` 切回本进程 ctx
- cleanup 顺序(UnmapMem → FreePhysical → ReleaseMemAddress):importer 无 imported VA
  (import 都失败);exporter barrier 后同序释放 16MB 主块 + 16MB 对照块

校验 pattern:`(rank<<28)|(tag<<24)|(idx&0xFFFFFF)`,tag 0x2/0x5 每步不同,防残留
数据假阳性(本测试虽不回读,仍写入以保持链路真实性与链上测试一致)。

## 运行方法
```
./run.sh                    # 默认 dev1→dev4(✗ 单元格,两种 import 预期失败)
A_DEV=4 B_DEV=1 ./run.sh    # ✗ 单元格反向(dev4→dev1)
A_DEV=1 B_DEV=6 ./run.sh    # 另一 ✗ 单元格(dev1→dev6)
A_DEV=0 B_DEV=2 ./run.sh    # 对照:可达跨卡(本进程 ctx import 预期成功,推翻预期,如实记录)
```

## 预期结果
- EnablePeerAccess(A/B)双向 ret=0(obs,✗ 单元格能打通 P2P 通路)
- 方式一(本进程 ctx import):预期 507899(EXPECTED-FAIL)
- 方式二(ctx 技巧 import):预期 507899(EXPECTED-FAIL)
- `[10] RESULT: PASS`:exporter 正向 1/3 全 OK(import 链路建得起来)+ 两种 import 方式
  都有结果(EXPECTED-FAIL / OTHER_ERR / UNEXPECTED 均为有效观测)即 PASS;
  若意外成功(UNEXPECTED)也 PASS 但显著标注"推翻预期"。

## ★实测结论(2026-08-25,CANN 9.1.0)

**推翻预期**:dev1→dev4 在 CANN 9.1.0 下 VMM import **可达**,两种导入方式都成功:

| 项目 | 预期 | 实际 |
|------|------|------|
| EnablePeerAccess(双向) | ret=0 | ret=0 ✓ |
| 方式一(本进程ctx import) | 507899 | **ret=0 成功** |
| 方式二(ctx技巧 import) | 507899 | **ret=0 成功** |

**结论**:原假设的"✗ 单元格 VMM 不可达边界"在当前环境(CANN 9.1.0)下**不成立**。
dev1→dev4 的 VMM import 与跨卡可达对行为一致。可能原因:
1. CANN 版本演进修复了早期驱动限制
2. 设备对分类(✗ 单元格)需重新验证

RESULT: PASS(意外成功,如实记录)。Cleanup 全部正常,无 D 态挂死。

### D 态挂死防护(沿用 03/04 修复)
本测试做跨卡 `aclrtDeviceEnablePeerAccess`,进程退出时若不显式拆除 P2P 状态会触发
驱动 SRCU 死锁(exit_mmap → synchronize_srcu 永久等待,SIGKILL 不可回收,耗尽
hugepages)。cleanup 在 `aclrtResetDevice` 前加 `aclrtDeviceDisablePeerAccess(peer)`
对称拆除(r0 拆 devB、r1 拆 devA);`run.sh` 加 `timeout 60` 兜底。详见
03_ipc_cross/README.md 的三条证据链。运行后检查 `ps aux|grep test` 不应有 D 态残留。
