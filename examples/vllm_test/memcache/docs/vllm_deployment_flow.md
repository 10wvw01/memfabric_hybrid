# vLLM 0.24 / MemCache 部署流程

测试用例、TTFT/P50 和常用术语见[测试说明](../../TEST_GUIDE.md)。

部署从指定上游仓库获取代码，使用仓库中的实现构建运行环境。

## 最近一次复测（2026-11-23，精简后用例集）

| 项 | 值 |
|---|---|
| 测试包 | `examples/vllm_test/memcache` |
| 部署目录 | `/tmp/opencode/memcache_full_20261120_064548`（MemCache + MemFabric + vLLM-Ascend 已构建安装） |
| 模型 | `/home/models/Qwen3.5-4B-W8A8-310P`（小模型，1 张卡即可） |
| 设备 | dev4,5（同卡 die-pair，host_shm/device_sdma 均可） |
| 协议 | `device_sdma` |
| 显存利用率 | `0.50` |
| workload | **5 passed in 27.80s**（prefill P50≈4180us @ seq=4096） |
| integration (rank2) | **4 passed, 1 failed in 760s**；失败项 `test_full_miss_baseline` 失败原因是 HCCL 端口 16666 未及时释放（详见 [troubleshooting.md](troubleshooting.md) 问题 8），其他 4 项（50%/100% 命中、缓存来源正确性、原生 prefix cache 对照）全部通过 |
| 集成测试关键耗时 | Hit P50≈150ms，Miss P50≈1375ms，hit/miss≈11%，外部命中复用 token 3840/4096 |

复测命令（已在 10006 共享环境验证）：

```bash
source /usr/local/Ascend/cann-9.2.1/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
export MMC_TEST_VLLM_MODEL_PATH=/home/models/Qwen3.5-4B-W8A8-310P
export MMC_TEST_VLLM_MODEL=Qwen3.5-4B
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
cd /home/l30023043/memfabric_hybrid/examples/vllm_test/memcache

# 1. 验证部署
python3 source_manifest.py verify --memcache-dir /tmp/opencode/memcache_full_20261120_064548 --require-vllm

# 2. workload（约 28 秒）
bash run_test.sh --memcache-dir /tmp/opencode/memcache_full_20261120_064548 \
    --type workload --device-ids 4,5 --log

# 3. integration（约 13 分钟）
bash run_test.sh --memcache-dir /tmp/opencode/memcache_full_20261120_064548 \
    --type integration --rank 2 --protocol device_sdma \
    --device-ids 4,5 --gpu-mem-util 0.50 --log
```

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
仅运行 MemCache 工作负载测试时可使用 `--memcache-only`；`--skip-build` 仅获取源码并记录版本。

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

## 6. 测试运行规范

本节总结共享设备环境下跑通完整测试流程的操作规范，避免因设备资源竞争或缓冲问题浪费时间。

### 6.1 启动前设备检查

测试前必须确认目标 die-pair 两块设备的空闲显存均大于 35 GB。310P3 单设备总显存约 44 GB，
vLLM 启动需要模型权重 + KV cache + 运行时工作区，显存不足会直接导致 `aclrtMallocPhysical` 返回 507899，
报错信息为 `Free memory on device (X/42.67 GiB) on startup is less than desired GPU memory utilization`。

检查脚本（解析 npu-smi 输出，列出每块设备的空闲显存）：

```python
import subprocess, re
out = subprocess.run(["npu-smi", "info"], capture_output=True, text=True).stdout
for line in out.splitlines():
    if re.match(r"\|\s+\d+\s+\d+\s+\|", line) and "/" in line and "AICore" not in line:
        m = re.search(r"(\d+)\s*/\s*(\d+)", line)
        if m:
            used, total = int(m.group(1)), int(m.group(2))
            parts = [p.strip() for p in line.split("|") if p.strip()]
            dev = parts[0].split()[1]
            print(f"dev={dev} free={total-used}MB")
```

同卡 die-pair 对应关系：dev0+1、dev2+3、dev4+5、dev6+7。选两块空闲显存均大于 35 GB 的设备组成 die-pair。
不要只看 `npu-smi info` 的进程列表为空就认为设备可用——hugepages 残留或孤儿 NPU 上下文也会占用显存，
必须看 `Memory-Usage` 列的实际已用值。

### 6.2 显存利用率与内存分配配置

共享设备环境下 `--gpu-mem-util` 默认 0.80 偏高，vLLM 启动时 `Free memory < total * gpu-mem-util` 会直接失败。
按设备空闲情况调整：

| 场景 | `--gpu-mem-util` | 说明 |
|------|------------------|------|
| 专用设备（无其他进程） | 0.80 | 默认值，充分利用显存 |
| 共享设备（有其他进程） | 0.40 ~ 0.50 | 留出模型权重和运行时空间 |
| 4-rank largecap | 0.60 ~ 0.70 | 按 README 默认 |

同时设置 `PYTORCH_NPU_ALLOC_CONF=expandable_segments:False` 关闭 PyTorch 可扩展内存段。
310P3 上 `expandable_segments:True`（vLLM 默认）会触发 `aclrtMallocPhysical` 返回 507899，
关闭后改用连续内存分配，规避该问题。完整环境变量：

```bash
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:False
export MMC_TEST_MAX_USED_MEMORY_RATIO=0.50  # 允许设备已用 50% 内存时启动（默认 0.25 在共享环境下过严）
```

### 6.3 分步执行与快速失败

不要一次性跑完整流程。分步执行，每步设超时，前一步失败立即停止：

1. **workload 先于 integration**：workload 不加载模型，约 1 分钟完成，能快速验证构建产物和 MetaService 是否正常。
   若 workload 失败，integration 必然失败，不必浪费时间。
2. **每步设 timeout**：workload 600 秒、integration 1800 秒。`timeout --preserve-status` 保留退出码。
3. **workload 失败时 abort**：不继续跑 integration。

```bash
# 第一步：workload（快速验证，~1 分钟）
timeout --preserve-status 600 python3 -u -m pytest \
    cases/python/memcache/workload/prefill_ttft_test.py \
    -v -s -m workload
# 若上面非零退出，停止
if [[ $? -ne 0 ]]; then echo "[ABORT] workload failed"; exit 1; fi

# 第二步：integration（完整集成测试，~17 分钟）
timeout --preserve-status 1800 bash run_test.sh \
    --memcache-dir /path/to/memcache \
    --type integration --rank 2 --protocol host_shm \
    --device-ids 0,1 --gpu-mem-util 0.40 --log
```

### 6.4 Python 输出无缓冲

pytest 的 `-s` 选项本应实时输出，但通过 `nohup` 或重定向运行时 stdout 会被块缓冲，
导致日志长时间不更新，无法判断测试是否卡住。必须加 `python3 -u` 强制无缓冲：

```bash
python3 -u -m pytest ... -v -s -m workload
```

`run_test.sh` 内部调用 pytest 时无法直接加 `-u`，此时通过监控 vLLM 日志文件
（`/tmp/vllm_integration_*.log`）判断进度：日志中出现 `Application startup complete` 表示 vLLM ready，
出现 `POST /v1/completions HTTP/1.1 200` 表示请求开始处理，
出现 `ERROR`、`EngineDeadError`、`507899`、`Free memory on device` 表示启动失败。

### 6.5 步骤间清理

每步结束后按精确 PID 清理残留进程和共享内存（勿用宽泛 `pkill -f` 误杀其他进程）：

```bash
ps -eo pid,cmd | grep -E "mmc_meta_service|pytest.*prefill_ttft|VLLM::|vllm serve|MetaService\.main" | grep -v grep
# 按输出的 PID: kill -9 <PID>
rm -f /dev/shm/mf_dram_*; sleep 5
```

vLLM 异常退出后 HBM 可能不立即释放，需等 10 秒以上再启动下一个实例，并确认显存恢复到 35GB 以上。

### 6.6 完整流程示例

```bash
source /usr/local/Ascend/cann-9.2.1/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
export MMC_TEST_VLLM_MODEL_PATH=/home/models/Qwen3.5-4B-W8A8-310P
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:False
export MMC_TEST_MAX_USED_MEMORY_RATIO=0.50
cd /home/l30023043/memfabric_hybrid/examples/vllm_test/memcache

# 1. 验证部署
bash verify.sh --memcache-dir "$MEMCACHE_DIR" --require-vllm

# 2. workload（失败则停止）
timeout --preserve-status 600 bash run_test.sh \
    --memcache-dir "$MEMCACHE_DIR" --type workload --device-ids 0,1 --log
[[ $? -ne 0 ]] && { echo "[ABORT] workload failed"; exit 1; }

# 3. 清理
ps -eo pid,cmd | grep -E "mmc_meta_service|pytest.*prefill_ttft" | grep -v grep | awk '{print $1}' | xargs -r kill -9
rm -f /dev/shm/mf_dram_*; sleep 5

# 4. integration
timeout --preserve-status 1800 bash run_test.sh \
    --memcache-dir "$MEMCACHE_DIR" --type integration --rank 2 \
    --protocol host_shm --device-ids 0,1 --gpu-mem-util 0.40 --log
```

### 6.7 vLLM-Ascend 集成补丁

`setup.sh` 自动 clone 的 vllm-ascend `trunk_for_escc_v24.0` 顶端可能不包含 310P MemCache 集成补丁。
README 前置条件第 3 条要求该补丁（commit `ca2909204`，"fix(310p): integrate MemCache with vLLM 0.24"），
未合入时需从 `fix/310p-memcache-v024-pr` 分支获取后重新 `pip install -e .`。

该补丁涉及三个 `compat_310p.h` 文件的 bf16 兼容修复（`chunk_fwd_o`、`chunk_gated_delta_rule_fwd_h`、
`chunk_gated_delta_rule_compute_wy`）。若编译报 `definition of type 'bfloat16_t' conflicts with typedef of the same name`，
检查这三个文件是否都加上了 `__BISHENG_CCEC__` 守卫。补丁完成后重新运行 `source_manifest.py write` 更新 manifest。

### 6.8 失败定位

按症状定位根因，详细排查见 [异常排查指南](troubleshooting.md)。

| 症状 | 退出码 | 原因 | 定位 |
|------|--------|------|------|
| join 后几秒被杀 | 137/124 | 驱动 devmm 映射失败 | `dmesg \| grep 'devmm.*Map address failed'` |
| prefill hang，无 TTFT Report | 124 | 设备并发争用致 D2D 死锁 | 查设备是否完全空闲 |
| integration 启动即退 | 1 | manifest 缺 vllm | 查日志 `must include vLLM` |
| 端口 5000/6000/8000 被占 | - | fork MetaService 残留 | `ps \| grep pytest.*prefill_ttft` |
| W8A8 模型崩溃 | - | CANN 版本低 | `echo $ASCEND_HOME_PATH`（应 9.2.1） |
| `KV transfer not supported for 310P` | - | vllm-ascend 缺补丁 | `git merge-base --is-ancestor ca2909204 HEAD` |
| `npu-smi info` 不返回 | 124 | 设备驱动异常 | `dmesg \| grep 'is not working'`，用 ACL API |
| `maxHbmSize must be 0` | - | host_shm 配置带 HBM | 用 `mmc-local-workload-hostshm-dramonly.conf` |
| `set_device ret=507033` | - | ACL 句柄残留 | 换卡或等几十秒 |
| `port 16666 have already been bound` | 1 | HCCL socket 未释放 | `ps \| grep vllm serve`，清理后等 10s 重跑 |

**定位步骤：** 看日志和退出码 → 查 dmesg 确认驱动 → 确认设备空闲（AICore 0%）→ 确认 CANN 版本和补丁（integration）→ 确认 manifest（integration）→ 按精确 PID 清理残留。

**基线：** prefill P50≈4000us，D2D 256MB≈50ms，workload≈38s，integration TP=2≈13min。
