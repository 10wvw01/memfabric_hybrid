## Why

PR !3（branch `310p-bm-bw`，已合入 dev-v1.2.0，merge commit 0320410e）为 310P 2-rank 场景补齐了两项核心能力，但尚无 OpenSpec 规格记录：

1. **`aclrtMemAttr` 枚举错位修复**：`dl_acl_api.h` 原为自编号（8 项），与 CANN `acl_rt.h` 真实编号（0~17）错位——值经 `aclrtMallocPhysical` 直传驱动，导致名为 `P2P_HUGE` 实发值 4（`ACL_HBM_MEM_HUGE`）、名为 `HBM_HUGE` 实发值 7（`ACL_DDR_MEM_P2P_NORMAL`，恒返 100000）。该缺陷曾使 PR !1 的全部实机验证事实上跑在 HBM_HUGE 上（天然实验），枚举对齐后 P2P 路径首次真正执行即暴露 P2P 分区上限。
2. **P2P_HUGE→HBM_HUGE 回退**：310P P2P 物理分区上限实测 ~4088MB（动态波动，超限返 207001），而 `ACL_HBM_MEM_HUGE` 从 AI 主池（~42GB）分配无此限制。无回退时 >4GB HBM 池必然建池失败。
3. **`benchmark/bm_310p` 独立带宽用例**：310P 双池（HBM AclVmm + DRAM HOST_SHM）十方向带宽此前无专用基准工具；通用版 `benchmark/bm` 为 910B GVA 单实体路径，硬编码 4GB 池且语义分叉，不适合直接改造。

> **补录说明**：本变更为补录（retroactive）——实现已在 e78a6faa..58e335c4 完成并经 PR !3 合入 dev-v1.2.0。本 change 依据《310p-2rank-bm-bw-测试计划.md》（仓库根）、git 历史与 MR !3 评论实测记录补齐规划工件并归档；tasks 全部标记为已完成。

## What Changes

- `dl_acl_api.h` 的 `aclrtMemAttr` 枚举改为与 CANN 9.1.0 `acl_rt.h` 严格对齐的全量 18 项（0~17），并注释编号错位风险（值直传驱动，错位即语义漂移）
- `HybmAclVmmSegment` 新增 `MallocPhysicalWithFallback`：`AclrtMallocPhysical(P2P_HUGE)` 返 207001 时 WARN（含 rankId/size/ret）后换 `ACL_HBM_MEM_HUGE` 重试一次；其他错误码不回退直接失败；`FillMemProp` 增加 memAttr 入参
- 新增 `benchmark/bm_310p`（`bm_perf_benchmark_310p`，复制自 `benchmark/bm`，通用版零改动）：双实体双池（entity0=HBM/SDMA 位、entity1=DRAM/HOST_SHM）、十方向按目的池类型路由 handle、CLI 新增 `-hbm-size/-dram-size`（默认 2GB，2MB 对齐校验）与 `-hm pinned|pageable`（host 缓冲口径，默认 pinned）、输出增加 Pool 列与 host_mem 标识、pinned 本地缓冲在 ResetDevice 前释放
- `barrier_util.h`/`common_def.h` 以 `benchmark/bm` 为唯一来源（CMake include 路径指向 `../bm`），消除 254 行逐字重复
- 文档：测试计划（含 F1-F3 实测裁决、M1 方案、附录 A/B/C 实测数据与复现命令）与能力矩阵 README 更新

## Capabilities

### Modified Capabilities

- `hybm/acl-vmm-hbm-pool`: 新增两条 Requirement——`aclrtMemAttr` 枚举与 CANN 严格对齐（值直传驱动的编号契约）；物理分配 P2P_HUGE→HBM_HUGE 回退（207001 触发、日志契约、回退范围限定）

### New Capabilities

- `benchmark/bm-310p-dual-pool`: 310P 双池十方向带宽基准能力——双实体建池与路由、host 缓冲 pinned/pageable 双口径与 copyType 语义、数据完整性校验、与通用版基准的共享头复用契约

## Impact

- **代码**：`src/hybm/csrc/under_api/dl_acl_api.h`（枚举对齐）、`src/hybm/csrc/mm/hybm_acl_vmm_segment.{h,cpp}`（回退）、`test/ut/mock/acl_stub.cpp` + `test/ut/testcase/hybm/mm/hybm_acl_vmm_segment_test.cpp`（stub 注入 + 新用例）、`benchmark/bm_310p/`（新增全套）+ `benchmark/bm`（零改动）、根 `CMakeLists.txt`（BUILD_TEST=ON 挂 bm_310p）
- **兼容性**：910B/910C/950 零行为变化（回退仅在 310P AclVmm 段内生效）；≤4GB 池行为不变（P2P_HUGE 直配）
- **回退范围**（经 issue #10 登记裁决）：P2P 句柄导入仅 die 间（同卡）共享；卡间受 PCIe P2P BAR 可寻址空间限制（单 endpoint <8G、两 die 共享一 endpoint、每 die 跨卡导入 <4G），HBM_HUGE + 跨卡 import 无实测证据
- **已知限制**：`ACL_HBM_MEM_HUGE` 官方文档标注仅 350/A3/A2 支持，310P3 实测成功属未文档化行为，存在 CANN 升级断供风险（issue #10 跟踪）
