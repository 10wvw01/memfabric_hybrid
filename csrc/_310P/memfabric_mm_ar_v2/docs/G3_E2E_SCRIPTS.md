# G3 E2E 实机验证脚本归档（2026-10-19）

测试目录 `../test/g3_e2e_scripts/` 归档 `V2_G3_QWEN_E2E_REPORT.md` 所用的全部运维/验证驱动脚本（原运行位置
`/tmp/opencode/g3/`，入库以便复现）。均为一次性实机验证驱动，非 CI 测试；
运行前须满足报告 §13 的部署前置（/dev/shm 扩容、CANN/MemFabric env、
`ASCEND_RT_VISIBLE_DEVICES=0,1`、设备显存 <2.5GB 排空）。

| 脚本 | 用途 | 对应报告章节 |
|---|---|---|
| `cold_cycles.sh` | 新进程冷启动 ×20（含 75s meta 区排空） | §5.3 / §10 |
| `gate_e_stability.sh` | Graph 模式 10 分钟长稳负载（首轮即挂死工况） | §8 |
| `gate_e_eager.sh` | Eager 模式长稳（复现对之 fused 重跑） | §8 |
| `gate_e_eager_stock.sh` | Eager 模式长稳 stock 对照（归因用） | §8 |
| `regression.sh` | T0/T1/T2/V1 回归驱动 | §9 |
| `extract.py` | 三会话 A/B 结果提取（stock/fused 指标对比） | §7 |
| `probe_graph_fix.py` | graph 编译旁路探针（单 serve + 请求 + 产物核查） | §4.6 |
| `fix_attempt_custom_op/` | Graph 修复尝试的代码快照（custom-op 路由 + 配套 UT），未合入主线，供 §12.3 遗留项实施时参考 | §4.6 / §12.3 |

注意事项：

1. `gate_e_*.sh` 的结果 JSON 汇总依赖 `OUT_DIR` 环境变量（脚本内已 export）。
2. `fix_attempt_custom_op/` 内为快照，不含构建产物；恢复方法：将
   `memfabric_mm_ar_v2_serve.py` 覆盖至 `vllm_ascend/_310p/ops/`、UT 覆盖至
   `tests/ut/_310p/`，并先清除 `~/.cache/vllm/torch_compile_cache`（报告 §11.4）。
3. 间歇性 aicore timeout（§8）未定位，长稳脚本可能触发不可恢复挂死，
   运行后注意按报告 §13 清理 `VLLM::` 残留进程。


> 整理后的执行入口：`../test/g3_e2e_scripts/regression.sh`。
> T0 独立 `v2_trial.py` 驱动并未随 G3 归档，只有显式设置
> `MF310P_V2_DIAG_DRIVER_DIR` 后才会运行其 G0/G1/G2；
> T1/T2 使用本仓库 `test/` 脚本。原始实机日志路径保留历史含义。
