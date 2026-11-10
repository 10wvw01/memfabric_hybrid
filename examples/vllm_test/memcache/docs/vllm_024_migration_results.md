# vLLM 0.24 / 310P / MemCache 迁移结果

## 结论

Qwen3.5-4B-W8A8-310P、Qwen3.6-35B-A3B-w8a8 的 TP=1 / TP=2，均完成全 miss、50% 命中、重复输入命中、聊天缓存来源正确性四项验证，合计16项通过。TP=4 按用户要求暂停。变更已推送，远程 PR 见下表。

vLLM 核心恢复原版0.24；适配位于 vLLM-Ascend、MemCache、MemFabric。保留共同固定地址机制，撤销独立动态选址、版本2元数据及 vLLM 核心校验例外。

## PR 分支与提交

| 内容 | 分支 | 提交 | PR |
|---|---|---|---|
| MemFabric运行库 | `fix/310p-fixed-vmm-context` | `6abf1066`、`4f0ca5f4` | [#31](https://gitcode.com/GDD_ESCC/memfabric_hybrid/merge_requests/31) |
| MemCache绑定和依赖 | `fix/python-copy-direction-defaults` | `3b12bb96`、`54d347c9` | [#1](https://gitcode.com/GDD_ESCC/memcache/merge_requests/1) |
| vLLM-Ascend适配 | `fix/310p-memcache-v024-pr` | `ca290920` | [#10](https://gitcode.com/GDD_ESCC/vllm-ascend/merge_requests/10) |
| 测试工具与文档 | `test/vllm024-memcache-310p` | 具体提交号见分支日志 | [#32](https://gitcode.com/GDD_ESCC/memfabric_hybrid/merge_requests/32) |

MemCache PR #1依赖MemFabric PR #31的修复提交。测试工具PR只修改`examples/vllm_test/memcache/`，与运行库源码提交分开。原`vllm-0.24-test`分支保留两部分修改的集成状态。

Ascend目标分支发布时已前进到`02d2a51d`。PR #10在独立worktree中适配最新目标，保留上游DFlash公共前缀最终校验，相关单测275 passed、10 skipped；该最新组合尚未重跑NPU端到端测试。下面的NPU结果仍对应原测试基线`b19127c7`加集成提交`a97b7320`。随附部署补丁对应已测版本，并通过PR #10源码的反向应用检查。

## 仓库基线

| 仓库 | 工作目录 | 基线 |
|---|---|---|
| vLLM | `/workspace/vllm-v024-test` | GDD_ESCC/vllm，v0.24.0，`ee0da84`；工作区干净 |
| vLLM-Ascend | `/workspace/vllm-ascend-v24-test` | GDD_ESCC/vllm-ascend，`trunk_for_escc_v24.0`，`b19127c7` |
| MemCache | `/workspace/memcache-v121-test` | GDD_ESCC/memcache，`trunk_for_escc_v1.2.1`，`2f6c7fe4` |
| MemFabric | `/workspace/worktree-temp/vllm-0.24-test` | GDD_ESCC/memfabric_hybrid，`trunk_for_escc_v1.2.0`，`24ad5d66`；worktree分支 vllm-0.24-test |

MemCache 的 MemFabric 子模块同步相同运行时代码。虚拟环境为 `/workspace/vllm024-venv`。安装的 `libmf_hybm_core.so` 与本轮构建产物逐字节一致，SHA256：`3db5b82a622af1eb2793de0e7ef2b4e8eb3a07e4dfe776e69b2ad863135a1d46`。MemFabric 全量 UT 产物位于独立的 build_ut/output_ut，没有覆盖运行库。

## 测试条件

- 10006服务器、memcache-tg容器；310P3、CANN9.2.1；TP=2为同卡双die 6/7，TP=1为设备6。
- `device_sdma`，每rank 2GiB HBM + 2GiB DRAM；`PYTORCH_NPU_ALLOC_CONF=expandable_segments:True`。
- 显存比例：Qwen3.5 TP=1/2为0.50；Qwen3.6 TP=2为0.75，TP=1为0.90。
- float16 + Ascend W8A8，eager模式，最大序列8192、批次token4096、最大序列数10；chunked prefill开启，FlashComm/MC2关闭，Mamba align、SSM状态float16。
- TTFT：4096个有效随机token输入、生成1 token，temperature=0；1次预热、10次串行测量。50%场景为5个预填输入+5个新输入；重复输入场景为10组先写后读。
- 基线和MemCache模式均开启本地prefix cache；每完成请求后执行 `/reset_prefix_cache?reset_external=false`，检查HTTP成功及引擎新增成功日志，重置发生在计时之外。TTFT识别实际token ID或文本，HTTP/SSE错误、空流均失败。
- 正确性：使用各模型聊天模板，关闭思考、正常EOS，最多生成8 token。A/B分别2827/5661 token，公共前缀2823 token。比对独立无connector基线、重复基线、本地复用基线及MemCache的seed/外部/本地/混合输出，并检查Prometheus命中计数增量。

P50为本轮样本中位数。下表记录共享服务器上的首token延迟实测结果。

## 性能与功能结果

单位ms。全miss差值为 `(MemCache miss P50 / baseline P50 - 1)`；最后两列来自同一重复输入配对用例。

| 模型 | TP | baseline P50 | MemCache miss P50 | miss差值 | 50%场景hit P50 | 配对miss P50 | 配对hit P50 | hit/miss | 四项结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Qwen3.5-4B | 1 | 3971.48 | 3962.37 | -0.23% | 473.75 | 4076.27 | 546.75 | 13.41% | 4/4通过 |
| Qwen3.5-4B | 2 | 2310.95 | 2342.95 | +1.38% | 428.30 | 2400.30 | 494.45 | 20.60% | 4/4通过 |
| Qwen3.6-35B-A3B | 1 | 5650.59 | 5729.70 | +1.40% | 748.73 | 5648.18 | 527.95 | 9.35% | 4/4通过 |
| Qwen3.6-35B-A3B | 2 | 3765.70 | 3556.53 | -5.55% | 1398.29 | 3610.64 | 468.65 | 12.98% | 4/4通过 |

缓存来源及生成一致性：

| 模型/TP | 纯外部命中token | 纯本地命中token | 混合命中token（本地+外部） | token输出 |
|---|---:|---:|---|---|
| Qwen3.5 TP=1/2 | 2688 | 2688 | 2688+2688 | 全部与基线一致 |
| Qwen3.6 TP=1/2 | 2560 | 2560 | 2560+2560 | 全部与基线一致 |

“100% hit”指重复请求占比；块大小、Mamba检查点和末块计算会使外部token复用比例低于100%。大模型配对日志显示外部加载3840/4096 token。

### 结果日志

日志统一保存在测试目录的 `retest_logs/vllm024-migration/`，被Git忽略；同名原始文件保留于 `/tmp/`。

| 内容 | 日志及pytest汇总 |
|---|---|
| 小模型TP=1四项 | `vllm024-final-qwen35-tp1-full.log`；4 passed，6 deselected，610.53s |
| 小模型TP=2四项 | `vllm024-final-qwen35-tp2-full.log`；4 passed，6 deselected，648.04s |
| 大模型TP=1四项 | `vllm024-final-qwen36-tp1-full.log`；4 passed，6 deselected，845.73s |
| 大模型TP=2全miss | `vllm024-final-qwen36-tp2-partial.log`；该项passed，后续项目被资源占用中断 |
| 大模型TP=2两项命中 | `vllm024-final-qwen36-tp2-hit-source.log`；两项passed，同文件早期来源测试因资源不足failed |
| 大模型TP=2最终聊天来源用例 | `vllm024-final-qwen36-tp2-source-final.log`；1 passed，9 deselected，240.98s |

大模型TP=2的四项通过结果由上述三个实际运行组成，不把同一历史日志内的失败来源用例记作通过。早期小模型分次实验已被最终完整四项结果取代。

## 单元回归与部署核对

- vLLM-Ascend提交前复核：171 passed；额外98 passed、10 skipped。日志 `vllm024-pr-ascend-ut.log`、`vllm024-pr-ascend-regression.log`。
- HTTP流校验：5 passed，覆盖文本token、特殊token ID、HTTP500、SSE error、空流；日志 `vllm024-pr-http-ut.log`。
- MemFabric定向回归：VMM9 passed、上下文1 passed。
- MemFabric全量ASAN：2068项中2058 passed、10 skipped（网络条件），无失败；行覆盖70.0%、分支46.4%。提交前复核日志为 `vllm024-pr-mf-full-ut.log`。
- 提交前重新执行完整 `bash script/run_ut.sh`，测试和覆盖率统计均完成，退出0；mockcpp与patch目录已恢复。格式整理校验了Python AST和C++非空白内容一致。
- 三个部署运行时补丁均通过反向应用检查，与工作区一致；主MemFabric与MemCache子模块三个运行时源文件一致；安装库cmp一致；部署脚本语法、源码diff检查通过。
- `setup.sh --skip-build --skip-install-libs --yes ...`在已提交的修复分支上复核通过，保持MemFabric子模块提交不变；基于指定分支的修复提交可重复部署。日志 `vllm024-pr-setup-check.log`。

## 已知限制及诊断记录

1. 固定基址`0x12da80000000`验证范围是本机设备6/7、上述两模型与TP=1/2；其他环境、跨卡及多池未验证。保留PR#23共同固定窗口和精确地址检查；所有rank必须使用相同基址与代码。未实现分布式地址协商。
2. 原始裸completion、ignore_eos=true的长输入出现token差异；无connector的同一输入重复计算也不一致，根因尚未解决。日志 `vllm024-final-qwen36-tp2-raw-baseline-unstable.log`。最终聊天用例保持逐token对比，验证范围是明确列出的输入，不属于通用模型精度验收。
3. 设备曾多次被其他容器的新任务占用，产生显存不足及一次HCCL分配失败；对应失败日志保留。按用户授权清理6/7，并临时监控本次窗口。初版监控误判环境变量缺失的自身子进程，已修正为完整进程树排除；该次失败单独归档。监控不属于产品补丁，测试结束已撤销。
4. TP=4按用户要求未执行；未完成多并发吞吐、长时间稳定性、HBM→DRAM eviction/rewarm大容量测试。历史largecap和可选多token辅助方法仍需单独迁移，具体见下文。

## 待提交修改清单

### MemFabric

| 文件 | 修改 |
|---|---|
| `src/hybm/csrc/common/hybm_define.h` | 固定窗口改为本机验证地址 `0x12da80000000`，保留所有 rank 相同地址的要求 |
| `src/hybm/csrc/under_api/dl_acl_api.h` | 判断线程是否已有正确设备和 ACL 上下文，缺少上下文时执行初始化 |
| `src/hybm/csrc/entity/hybm_entity_default.cpp` | 校验线程当前设备及非空 ACL 上下文，缺失或设备不符时调用 SetDevice 初始化 |
| `test/ut/testcase/hybm/hybm_entity_default_test.cpp` | 补充线程上下文回归 |
| `test/ut/testcase/hybm/mm/hybm_vmm_based_segment_test.cpp` | 固定地址精确预留及地址不符释放回归 |
| `examples/vllm_test/memcache/` | 配置、部署补丁、启动保护、测试用例、HTTP 流校验、迁移报告，清除 vLLM 核心补丁依赖；部署检查指定的 MemCache 分支 |

固定基址调整是当前机器部署的候选补丁，跨环境采用同一常量前仍需验证。动态分配及元数据格式变化不在最终修改中。

### MemCache

| 文件 | 修改 |
|---|---|
| `src/memcache/csrc/python_wrapper/pymmc.cpp` | 10 处 Python 参数默认值把未注册复制方向枚举显式转为 `int32_t`，解决模块导入问题 |
| `3rdparty/memfabric_hybrid` | 本地同步上面的 MemFabric 运行时代码；发布时应引用审核合入后的 MemFabric 提交 |

`debug/` 为本地实验材料，不属于拟提交产品补丁。

### vLLM-Ascend

| 文件 | 修改 |
|---|---|
| `csrc/moe/chunk_fwd_o/op_kernel/arch20/compat_310p.h` | CANN 兼容编译 |
| `csrc/moe/chunk_gated_delta_rule_fwd_h/op_kernel/arch20/compat_310p.h` | 相同的 CANN 兼容编译修正 |
| `vllm_ascend/_310p/model_runner_310p.py` | 310P 开放 AscendStoreConnector + MemCache 组合 |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py` | 初始化 ACL 设备，调整协议读取和缓存区域注册日志 |
| `vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py` | 与 0.24 的 hash block size 解析一致；处理未满足 2 MiB 起始对齐的缓存区域 |
| `vllm_ascend/patch/platform/patch_kv_cache_coordinator.py` | 复用包含 Mamba 状态的共同有效前缀匹配，返回每缓存组命中 token 数 |
| `tests/ut/patch/platform/test_prefix_cache_cp_patches.py` | 增加 4 组混合缓存匹配回归 |

### vLLM

无源码修改，工作区保持 `ee0da84` 原版。Mamba align 使用原版支持的本地 prefix cache 开启配置。

## 测试用例与目的

| 测试 | 条件和目的 |
|---|---|
| 全 miss 对比 | 相同启动参数，分别关闭/开启 MemCache connector，10 个不同 4096-token 输入；比较 P50 TTFT 与相对差值 |
| 50% hit | 预填充后混合 5 个命中与 5 个 miss，重置本地 prefix，检查外部缓存命中与 TTFT 分组 |
| 100% hit | 先写后读相同输入，重置本地 prefix，比较 miss/hit TTFT |
| 缓存来源正确性 | Paris/Berlin 自然语言 A/B 两段输入，生成 8 token；与独立基线 token ID 比较，并检查本地/外部命中计数增量 |
| HTTP 流校验 | 模拟正常文本、特殊 token ID、HTTP 500、SSE error、空响应，避免把错误或空流计作低延迟 |
| 固定窗口单元测试 | 模拟精确地址成功及返回其他地址，验证拒绝并释放异常地址 |
| ACL 线程上下文单元测试 | 验证线程设备已有但上下文缺失时仍初始化 |

这些是串行功能与首 token 延迟测试；没有完成多并发吞吐、长时间稳定性及 TP=4 基准。历史 `large_capacity_vllm_test.py` 仍有旧启动参数；可选多 token 辅助方法 `measure_e2e_full_latency` 仍按 SSE 事件统计，没有使用本轮 TTFT 流校验逻辑。这两条历史路径未计入当前迁移结果。


## 本次修改文件

`/workspace/worktree-temp/vllm-0.24-test`：

```text
examples/vllm_test/memcache/README.md
examples/vllm_test/memcache/cases/config/mmc-local-integration-1rank.conf
examples/vllm_test/memcache/cases/config/mmc-local-integration-4rank.conf
examples/vllm_test/memcache/cases/config/mmc-local-integration.conf
examples/vllm_test/memcache/cases/python/memcache/integration/vllm_http_test.py
examples/vllm_test/memcache/cases/python/memcache/integration/vllm_integration_test.py
examples/vllm_test/memcache/docs/error_analysis_report.md
examples/vllm_test/memcache/docs/vllm_024_migration_results.md
examples/vllm_test/memcache/docs/vllm_ttft_test_issues.md
examples/vllm_test/memcache/docs/vllm_ttft_test_report.md
examples/vllm_test/memcache/patches/memcache_modified_files.patch
examples/vllm_test/memcache/patches/memfabric_310p_vmm_context.patch
examples/vllm_test/memcache/patches/vllm_ascend_310p_kv_transfer.patch
examples/vllm_test/memcache/run_test.sh
examples/vllm_test/memcache/setup.sh
examples/vllm_test/memcache/start_service.sh
src/hybm/csrc/common/hybm_define.h
src/hybm/csrc/entity/hybm_entity_default.cpp
src/hybm/csrc/under_api/dl_acl_api.h
test/ut/testcase/hybm/hybm_entity_default_test.cpp
test/ut/testcase/hybm/mm/hybm_vmm_based_segment_test.cpp
```

`/workspace/memcache-v121-test`：

```text
3rdparty/memfabric_hybrid
src/memcache/csrc/python_wrapper/pymmc.cpp
```

`/workspace/vllm-ascend-v24-test`：

```text
csrc/moe/chunk_fwd_o/op_kernel/arch20/compat_310p.h
csrc/moe/chunk_gated_delta_rule_fwd_h/op_kernel/arch20/compat_310p.h
tests/ut/patch/platform/test_prefix_cache_cp_patches.py
vllm_ascend/_310p/model_runner_310p.py
vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/memcache_backend.py
vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py
vllm_ascend/patch/platform/patch_kv_cache_coordinator.py
```

`/workspace/vllm-v024-test`：

```text
(clean)
```
