# vLLM 0.24 / MemCache 部署流程

部署从指定上游仓库获取代码，使用仓库中的实现构建运行环境。

## 1. 准备环境

在独立 Python 环境中准备 vLLM 0.24、CANN、torch 和 torch-npu，以及所需模型。
vLLM 使用原版 0.24 源码；通过 `--vllm-dir` 指定该源码目录。
执行 CANN 的 `set_env.sh`，构建和测试使用同一个 Python 环境。

## 2. 获取上游源码

| 仓库 | URL | 分支 |
|---|---|---|
| MemCache | `https://gitcode.com/GDD_ESCC/memcache.git` | `trunk_for_escc_v1.2.1` |
| MemFabric | `https://gitcode.com/GDD_ESCC/memfabric_hybrid.git` | `trunk_for_escc_v1.2.0` |
| vLLM-Ascend | `https://gitcode.com/GDD_ESCC/vllm-ascend.git` | `trunk_for_escc_v24.0` |

MemFabric 位于 MemCache 的 `3rdparty/memfabric_hybrid` 目录。
`setup.sh` 在目录不存在时克隆，已有目录先检查未提交源码修改，再 fetch、切换指定分支并快进更新。
脚本不执行源码补丁应用，也不丢弃未提交文件。

## 3. 构建安装

在测试包目录运行：

```bash
bash setup.sh --memcache-dir /workspace/memcache \
  --vllm-dir /workspace/vllm --vllm-ascend-dir /workspace/vllm-ascend --yes
```

脚本初始化构建依赖，构建 MemCache 及其 MemFabric 依赖，安装本次生成的 wheel 和运行库，
并安装指定源码目录中的 vLLM-Ascend。构建前清理已有算子生成物，确保分支更新后的头文件重新参与编译。
vLLM 0.24 在部署前准备。
仅运行 Layer 2 时可使用 `--layer2-only`；`--skip-build` 仅获取源码并记录版本。

## 4. 验证部署

```bash
bash verify.sh --memcache-dir /workspace/memcache \
  --vllm-dir /workspace/vllm --vllm-ascend-dir /workspace/vllm-ascend
```

`retest_logs/source_manifest.json` 保存源码目录、分支、提交号及关键构建文件 SHA256。
验证会检查版本记录、Python 实际加载路径、MemFabric 运行库与构建产物是否一致，以及 MetaService 是否生成。

## 5. 执行集成测试

```bash
export MMC_TEST_PROMPT_TOKENS=4096 MMC_TEST_DECODE_MAX_TOKENS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export MMC_TEST_VLLM_MODEL_PATH=/workspace/models/Qwen3.5-4B-W8A8-310P
export MMC_TEST_VLLM_MODEL=Qwen3.5-4B
bash run_test.sh --memcache-dir /workspace/memcache \
  --type integration --rank 1 --protocol device_sdma --device-ids 4 --gpu-mem-util 0.50 --log
bash run_test.sh --memcache-dir /workspace/memcache \
  --type integration --rank 2 --protocol device_sdma --device-ids 4,5 --gpu-mem-util 0.50 --log

export MMC_TEST_VLLM_MODEL_PATH=/workspace/models/Qwen3.6-35B-A3B-w8a8
export MMC_TEST_VLLM_MODEL=Qwen3.6-35B-A3B
bash run_test.sh --memcache-dir /workspace/memcache \
  --type integration --rank 1 --protocol device_sdma --device-ids 4 --gpu-mem-util 0.90 --log
bash run_test.sh --memcache-dir /workspace/memcache \
  --type integration --rank 2 --protocol device_sdma --device-ids 4,5 --gpu-mem-util 0.75 --log
```

选择可用设备；TP=2 示例使用同卡两个 die。模型、测量次数、输入与输出长度按测试配置设置。
测试结果同时记录源码提交、设备组合、模型、协议、命中情况和 TTFT 数据。
完整条件及本轮 16 项通过的性能数据见[迁移结果](vllm_024_migration_results.md)。
