# 17_aicore_aicpu_overlap — E0a：MemFabric 构建的协同编排 kernel CP1 执行验收

> 上游计划：`/310p-aicore-aicpu-开发计划.md` P0-E0a。
> 迁移源：xcclpp main `examples/tutorials/aicpu-aicore-310p3/`（AICore×AICPU 协同，MIT）。

## 目的

验证三件事（E0a 出口条件）：

1. **MemFabric 侧构建**：fork 的 AICPU 编排 kernel（`hybm_overlap_kernel.cc`）能用
   hybm_kernel 同款 hcc 工具链编译出可部署的 .so；
2. **部署链复刻**：deployer（CUSTAICPUKernel mode=1 迁移）+ mini_kfc（设备内复制）+
   验证门在本机走通；
3. **CP1 内执行**：`HybmOverlapRun` 真正在 `aicpu_scheduler`（CP1）进程内运行，且
   SQE 直投闭环成立（TSFW 消费 + SDMA 数据搬运 + 标志 SQE 到达）。

本实验为**单进程/单设备**形态（邮箱预填 + mode13 role0，数据与标志均在本地设备内存），
不含 AICore kernel 与跨卡——那是 E0c 的范围。

## 文件索引

| 文件 | 说明 |
|------|------|
| `hybm_overlap_contract.h` | wire 契约（host ↔ kernel 共享，static_assert 锁布局；fork 自 xcclpp overlap_wire.hpp） |
| `aicpu_kernel/hybm_overlap_kernel.cc` | AICPU 编排 kernel（mode13 闭环/mode14 流水状态机；fork 自 xcclpp_overlap_aicpu.cpp，重命名 HybmOverlap*） |
| `deployer.cpp` | CUST 迁移部署工具（fork，functionName 改 HybmOverlapRun） |
| `mini_kfc.cpp` | KFC 设备命令通道工具（fork，行为逐字节一致） |
| `probe_e0a.cc` | E0a 验收探针（六项判据，见下） |
| `probe_e0b.cc` | E0b 池 GVA 探针（双 rank 双 die 池 + 两轮变体：轮A 纯本地src→peer池GVA / 轮B own池GVA→peer池GVA） |
| `Makefile` | hcc kernel + g++ 工具/探针（与 src/hybm/ops/hybm_kernel 同工具链） |
| `run_e0a.sh` | 一键：编译 → 部署（纪律见计划附录 A.4）→ 探针验收 |
| `run_e0b.sh` | E0b 一键：部署保障（验证门+按需重部署）→ 双 rank 运行 → 结果判读 |

## 用法

```bash
source /usr/local/Ascend/cann/set_env.sh   # 或 bash run_e0a.sh 内置 source
bash run_e0a.sh 0                          # E0a: 部署到 dev0 并跑验收探针
bash run_e0b.sh 0 1                        # E0b: 双 rank(同卡 die 对)池 GVA 探针
bash run_e0a.sh clean 0                    # 清理设备侧文件 + 杀 deployer
```

环境变量：`KVER`（kernel 版本，默认 1，**改 kernel 必 bump**）、`ALIVE_SECS`（deployer
保活，默认 900s）。

## 验收判据（probe_e0a 六项，全过 = E0a PASS）

| # | 判据 | 证明什么 |
|---|------|---------|
| ① | result.magic == 0xA1C0DE01 | kernel 真正在 CP1 执行过 |
| ② | mainRet == 0 且 stage == STAGE_DONE | 编排状态机走完全程（活查询/ssid/SQE 对/doorbell 全链） |
| ③ | headAfter != headBefore | TSFW 消费了 SQE（硬件执行证据，非假成功） |
| ④ | flagDone == 哨兵魔数 | 标志 SQE 的 8B 搬运到达（SQE 对配方正确） |
| ⑤ | dst 与 src 逐字一致 | 数据 SQE 搬运正确（SMMU ssid 翻译有效） |
| ⑥ | ssid 诊断戳 != 0xFFFF | `drvMemSmmuQuery` 实时查询成功 |

## 术语注释表

| 术语 | 说明 |
|------|------|
| CP1 | 设备侧 aicpu_scheduler 进程，type=2(AICPUKernel) kernel 的执行宿主 |
| CUST 通道 | CUSTAICPUKernel mode=1：H2D 迁移 .so 落 `/home/CustAiCpuUser/`（不经验签） |
| KFC | KFCKernel type=5 的 dlsym 回退通道：host 一条 launch，设备 OS 执行 shell 命令 |
| CP 流 | `ACL_STREAM_DEVICE_USE_ONLY` 流，其 SQ 注册进 CP1 进程 trs 表（kernel 内可查可投） |
| SQE 对 | 数据 SQE + 标志 SQE 成对提交一次 doorbell，FIFO 保序保证标志到即数据到 |
| 验证门 | `test -f` + `grep -q <符号>`：部署后确认目标文件存在且含入口符号 |
| sync= | mini_kfc 输出的命令退出码语义（0=成功，非零→507018 形式回传；X9 实测勘正） |

## 结论

**E0a PASS（2026-09-10，dev0，一次通过）**：

| 判据 | 实测值 | 结果 |
|------|--------|------|
| ① magic | 0xA1C0DE01 | ✓ |
| ② mainRet / stage | 0 / 5 (STAGE_DONE) | ✓ |
| ③ headBefore → headAfter | 0 → 2（SQE 对两条全被 TSFW 消费） | ✓ |
| ④ flagDone | == 哨兵魔数 0xC0FFEE11 | ✓ |
| ⑤ 数据校验 | 1024/1024 字一致，mismatch=0 | ✓ |
| ⑥ ssid 实时查询 | 2（pad 低 16 位） | ✓ |

附注：CP 流 streamId=240 / sqId=241 / dieId=0；部署链（deployer load/getfunc=0 →
KFC cat/chmod → 双验证门）全程纪律化执行无异常。**E0a 出口条件达成**：MemFabric 侧
hcc 构建 + 部署链复刻 + CP1 内 SQE 直投闭环三项全部验证通过。

**E0b PASS（2026-09-10，dev0↔dev1 双 die 32MB HBM 池，一次通过）——架构成立性裁决**：

| 变体 | SQE src/dst | 结果 | 证明 |
|------|------------|------|------|
| 轮A | src=纯本地设备内存，dst=**peer 池 GVA**（AclVmm 导入映射） | PASS（headAfter 0→2，数据 1024/1024 一致） | **导入 VA 可被 SDMA 经 SMMU ssid 翻译** |
| 轮B | src=**own 池 GVA**（本端窗口 VA），dst=**peer 池 GVA** | PASS（headAfter 2→4，数据 1024/1024 一致） | 本端窗口 VA 同样可译——真实使用形态全通 |

GVA 对称基址 0x12c140000000（与 07 实测一致）；ssid=2 实时查询；标志 SQE 8B 到达。
**结论：AclVmm 池 GVA 可直接作 SQE src/dst，数据面架构成立，无需 Plan B（段层 IPC Case A
导入模式不启动）。** AICPU 全程未解引用池 VA（D9 纪律），池地址仅出现在 SQE 字段。

## 与后续实验的关系

- **E0b**（池 GVA 作 SQE src/dst）：本探针的 dst/src 换成 AclVmm 池内 GVA —— 数据面
  架构成立性的最终裁决；
- **E0c**（池上协同最小复现）：加入 bisheng 编译的 AICore kernel（写邮箱/轮询 flag）+
  双 die 池，八步语义全链；
- **P1-WP1**：本目录 kernel/contract 迁入 `src/hybm/ops/hybm_kernel/` 主构建。
