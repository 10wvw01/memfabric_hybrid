# 310p_sdma_stability：310P 设备发起 SDMA 长稳压测（真机集成测试）

## 场景

Atlas 300I Duo（Ascend310P3）单卡双 die 2-rank SHM 对称内存池上，对**设备发起 SDMA
协同链路**（AICore 邮箱原语 × AICPU 搬运 kernel 分轮运行）做长稳验证。两项内容均为
serve 长驻场景的防回归（内容迁自 examples/08 的 ③④，随检视意见移入 test/）：

1. **持续收发长稳**：单 chunk 闭环（r0 产+signal → r1 wait+INC+回件 → r0 wait）逐轮
   运行，跨搬运 kernel 换轮边界（默认每 15s 到点自退、监督线程重拉、环+游标零丢失），
   断言：
   - 总时延上界（若某轮 wait 落入无消费死等，轮询预算会撑爆总时延 → FAIL）
   - 终态数据逐字校验（pattern + INC）
   - 单轮最大时延应为换轮间隙量级（~100ms），历史实测 avg 1.34ms/轮
2. **全设备同步共存探针**：`AclrtSynchronizeDeviceWithTimeout(90s)`（对齐 torch_npu
   workspace 分配器内部同步的量级）在分轮运行下**必须能完成**——永不退出的搬运 kernel
   会把该同步拖到超时（507017/507046，serve 实测死因），本探针即该死因的本地复现防线。

## 使用能力

`smem_shm_create(SDMA)` 建池 → AICore kernel 内 `smem_shm_sdma_signal/wait/quiet`
（设备侧自驱，host 仅起 kernel/收结果）。

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1，双 die 空闲
- MPI（与 08 示例相同的两套自动识别约束：编译与启动同一套）
- 协同编排 kernel 三件套（主构建产出，初始化自动部署）

## 运行

```bash
bash run.sh                    # 默认 30000 轮（约 46 秒）+ 设备同步探针
STRESS_ROUNDS=300000 bash run.sh --run-only   # 5 分钟量级长稳
TEST_DEVICE_BASE=4 bash run.sh --run-only     # 用 dev4/dev5（避开占用设备）
```

期望输出：双方 `①长稳×N: ... (avg ~1.3ms/轮, max ~100ms=换班间隙)`、
`②设备同步: ...ms PASS`、`=== 310P SDMA 长稳 rN PASS ===`。

## 验收标准

1. ①长稳总时延 ≤ 轮数×20ms + 30s 上界；终态数据逐字校验通过
2. ②设备同步 90s 预算内完成（等待 = 当前轮剩余时长，属正常）
3. `destroy`/`uninit`/`MPI_Finalize` 干净退出，退出码 0

## 与相关内容的关系

- **examples/08**：能力演示（多 chunk 流水/串行对照/inline reduce），长稳与探针测试
  内容已移入本目录
- **机制与现场数据**：`docs/310p/机制档案.md`（SQE 配方/换轮/工作区）、
  `docs/310p/平台事实.md`（507017 死因链与分轮修复定案）
