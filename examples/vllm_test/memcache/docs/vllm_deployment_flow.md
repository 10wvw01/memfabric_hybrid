# memcache 路线：vLLM TTFT 测试环境部署流程

本文档整理 memcache 路线 vLLM 测试部署的完整流程，按执行顺序拆解每一步的具体命令、命令作用与关键参数含义。

部署由两个脚本串联：`setup.sh`（拉源码 + 补丁 + 编译）和 `run_test.sh` / `start_service.sh`（起 MetaService + 起 vLLM + 跑 pytest）。

## Step 1 — 拉取 memcache 仓库并切到基准 tag

由 `setup.sh:118-160` 执行：

```bash
git clone https://gitcode.com/Ascend/memcache.git /path/to/memcache
cd /path/to/memcache
git checkout v1.2.1
```

- 基准固定在 tag `v1.2.1`（`setup.sh:24`）。
- 如果目录已是 git 仓库且当前不在 v1.2.1，脚本只 warn 不强制切换，避免覆盖用户本地修改。

## Step 2 — 打 memcache 自身补丁（2 个文件）

由 `setup.sh:165-191` 执行 `git apply patches/memcache_modified_files.patch`。主要改动点：

1. **`python_wrapper/CMakeLists.txt`**：`set(PYTHON_EXECUTABLE "python3")` → `find_program(PYTHON_EXECUTABLE NAMES python3 python)`。原硬编码 `python3`，部分环境只有 `python` 软链接或装在非标准路径，CMake 探测 site-packages 失败。
2. **`python_wrapper/pymmc.cpp`（10 处）**：所有 `py::arg("direct") = SMEMB_COPY_*` 改成 `= static_cast<int32_t>(SMEMB_COPY_*)`，覆盖 10 个方法 × 3 种枚举（`G2H`/`H2G`/`G2L`）。`SMEMB_COPY_*` 是未向 pybind11 注册的 C 枚举，`import` 时转 Python 对象失败抛 `ImportError: type not registered yet?`。`int32_t` 是已注册类型，编译不受影响（枚举隐式转 int），问题只在运行时 import 触发。

## Step 3 — 初始化 memfabric_hybrid 子模块 + 切 dev-v1.2.0 分支

由 `setup.sh:196-205` 调 `fix_memfabric_hybrid()`（`setup.sh:77-103`）：

```bash
cd $MEMCACHE_DIR
git submodule update --init 3rdparty/memfabric_hybrid
cd 3rdparty/memfabric_hybrid
git stash && git reset --hard && git clean -fd          # 清残留
git remote add fork https://gitcode.com/GDD_ESCC/memfabric_hybrid.git
git fetch fork dev-v1.2.0 && git checkout -B dev-v1.2.0 fork/dev-v1.2.0
```

**为什么切 fork 分支**：`.gitmodules` 指向 `Ascend/memfabric_hybrid.git` master，但 `dev-v1.2.0`（在 `GDD_ESCC` fork 上）已合入 IsSameCard bug 修复（die-pair 同卡判断错误导致 P2P 路径选错）、107002 修复（store.init 偶发错误码）、207001 P2P_HUGE 回退（`AclrtMallocPhysical` P2P 分区 OOM 时回退 `ACL_HBM_MEM_HUGE`），master 上还没有。

**为什么 submodule update 后要重跑**：`git submodule update` 会 reset 子模块回父仓库记录的 commit，分支切换全丢。`fix_memfabric_hybrid` 在 `setup.sh` 里调两次（Step 3 初始化后 + Step 5 构建前），每次先清干净再重切。

## Step 4 — 打 vLLM-Ascend 补丁（3 个文件，Layer 3 需要）

由 `setup.sh:210-263` 执行 `git apply patches/vllm_ascend_310p_kv_transfer.patch`。用 `--skip-patch-vllm` 可跳过（仅 Layer 2 不需要 vLLM）。主要改动点：

1. **`_310p/model_runner_310p.py` ~L700**：`raise ValueError("KV cache transfer is not supported for 310P.")` → `logger.info(...)`。上游因 310P 不支持标准 `AscendConnector`（HCCS 路径）而硬编码阻断所有 KV transfer；但 `AscendStoreConnector` 走 memcache + MemFabric SDMA，不需要 HCCS，310P 可用。
2. **`ascend_store/backend/memcache_backend.py`**（4 处）：
   - 拆 `_is_device_sdma()` 为 `_get_protocol()` + `_is_host_shm()`，原 `MMC_LOCAL_CONFIG_PATH` 未设置时 `raise ValueError` 改返回空串——支持 host_shm 协议且 import 时不崩。
   - 新增 `_resolve_physical_device_id(local_rank)`：从 `ASCEND_RT_VISIBLE_DEVICES` 映射 local_rank→物理设备 ID。`VISIBLE_DEVICES=6,7` 时 local_rank 0 对应物理 6，HYBM SDID 查找需物理 ID。
   - init 时显式 `acl.init()` + `acl.rt.set_device(local_rank)`：MemcacheBackend 在独立线程 init，ACL context 可能未继承，SDMA 操作要求有效 ACL context。
   - `register_buffer` 加逐 buffer 返回码检查 + 异常捕获：原不检查返回值，静默失败后 SDMA 传输才报错，错误现场丢失。
3. **`ascend_store/pool_worker.py` ~L758**：2MB 对齐 `assert new_start >= storage_key` → `if ... : logger.warning(...); continue`。hybrid 模型（Qwen3.6）子张量 storage_offset 导致对齐后地址越界触发 assert 崩溃；对齐只是优化（大页映射），跳过不影响正确性。

## Step 5 — 构建 memcache + 安装 wheel + 替换 MemFabric 库

由 `setup.sh:268-325` 执行：

```bash
cd $MEMCACHE_DIR

# 子模块切换前先 stash 掉 Step 3 的补丁残留
cd 3rdparty/memfabric_hybrid && git stash
cd $MEMCACHE_DIR

# 替换 github 子模块为 gitcode 镜像（受限网络可达性）
sed -i 's|https://github.com/biaks/prometheus-cpp-lite.git|https://gitcode.com/Abstrey/prometheus-cpp-lite.git|' .gitmodules
sed -i 's|https://github.com/sinojelly/mockcpp.git|https://gitcode.com/Abstrey/mockcpp.git|' .gitmodules

# 只初始化构建所需子模块（跳过 test/ 子模块，BUILD_TEST=OFF）
git submodule update --init \
    3rdparty/log/spdlog \
    3rdparty/nlohmann/json \
    3rdparty/msgpack-c \
    3rdparty/libzmq \
    3rdparty/prometheus-cpp-lite

# submodule update 又把 memfabric_hybrid reset 了，重新切回 dev-v1.2.0
fix_memfabric_hybrid

# 构建（RELEASE，不开 UT，约 15-20 分钟，207 个编译目标）
bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF

# 安装 Python wheel
pip install --force-reinstall --no-deps output/memcache/wheel/memcache_hybrid-*.whl

# 用新编译的 libmf_*.so 替换 site-packages 中已安装的库
HYBRID_LIB_DIR=$(python3 -c "import memfabric_hybrid, os; print(os.path.dirname(memfabric_hybrid.__file__)+'/lib')")
cp 3rdparty/memfabric_hybrid/output/hybm/lib64/libmf_hybm_core.so $HYBRID_LIB_DIR/
cp 3rdparty/memfabric_hybrid/output/smem/lib64/libmf_smem.so       $HYBRID_LIB_DIR/

# 验证 SDID fallback 修复已编入 .so
strings $HYBRID_LIB_DIR/libmf_hybm_core.so | grep "using 0, may affect"
```

- `build_and_pack_run.sh` 是 memcache 自己的 wrapper（类似 memfabric_hybrid 的那个），内部跑 CMake。
- 替换 `.so` 是因为 wheel 里打包的库可能是旧版，而 dev-v1.2.0 的最新产物需要手动覆盖到 Python 包目录，否则 `import memfabric_hybrid` 加载的还是旧库。

## Step 6 — 拷贝辅助脚本到 memcache 仓库

由 `setup.sh:330-337`：

```bash
mkdir -p $MEMCACHE_DIR/debug/test
cp aux_scripts/* $MEMCACHE_DIR/debug/test/
```

- 主要是 `clean_stale_env.sh`（测试前清理残留进程和 IPC 资源）和几个故障复现脚本（`repro_207001_*`、`repro_sdid_207000.*`）。

## Step 7 — 启动 MetaService（KV 存储元服务，必须先于 vLLM）

由 `start_service.sh:93-108` 或 `run_test.sh:332-349`：

```bash
# 先设环境
export ASCEND_HOME_PATH=${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.1.0}
export ASCEND_RT_VISIBLE_DEVICES=6,7
export MMC_MEMCACHE_DIR=$MEMCACHE_DIR
export MMC_META_CONFIG_PATH=$MEMCACHE_DIR/config/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=$PKG_DIR/cases/config/mmc-local-integration.conf
export LD_LIBRARY_PATH="/usr/local/Ascend/driver/lib64:$ASCEND_HOME_PATH/lib64:$MEMCACHE_DIR/output/memcache/lib64:$(python3 -c 'import memcache_hybrid,os;print(os.path.dirname(memcache_hybrid.__file__)+"/lib")'):$(python3 -c 'import memfabric_hybrid,os;print(os.path.dirname(memfabric_hybrid.__file__)+"/lib")')"

# 启动 meta 服务（后台）
$MEMCACHE_DIR/output/memcache/bin/mmc_meta_service > /tmp/mmc_meta.log 2>&1 &
sleep 3
```

- **为什么必须先起**：vLLM 的 `AscendStoreConnector` 启动时会连 `tcp://127.0.0.1:5000`（meta_service_url）注册 rank、分配 store 句柄；meta 没起，connector init 直接失败。
- `MMC_META_CONFIG_PATH` 指向 `mmc-meta.conf`（meta 服务自身配置），`MMC_LOCAL_CONFIG_PATH` 指向 `mmc-local-integration.conf`（每个 rank 的本地存储配置：protocol、HBM/DRAM size、world_size）。
- `run_test.sh` 会先 `is_port_listening 5000` 检查，已在跑就不重启；自己起的 meta 在脚本退出时 trap kill。

## Step 8 — 启动 vLLM serve（带 AscendStoreConnector）

由 `start_service.sh:119-130`：

```bash
vllm serve /home/models/Qwen3.6-35B-A3B-w8a8 \
  --host 127.0.0.1 --port 4077 \
  --tensor-parallel-size 2 --quantization ascend \
  --served-model-name Qwen3.6-35B-A3B --trust-remote-code \
  --dtype float16 --max-num-seqs 10 --max-num-batched-tokens 4096 \
  --max-model-len 8192 --gpu-memory-utilization 0.80 \
  --enforce-eager --enable-chunked-prefill \
  --mamba-ssm-cache-dtype float16 --mamba-cache-mode align \
  --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}' \
  --no-enable-prefix-caching --prefix-match-unit 128 \
  --kv-transfer-config '{"kv_connector":"AscendStoreConnector","kv_role":"kv_both","kv_connector_extra_config":{"backend":"memcache"}}' \
  > /tmp/vllm_memcache.log 2>&1 &
```

关键参数答疑：

- `--quantization ascend`：用 ascend 量化路径加载 W8A8 模型。
- `--enforce-eager`：禁用 graph mode（310P3 graph 对 hybrid 模型支持不稳）。
- `--enable-chunked-prefill`：分块 prefill，避免长 prompt 一次性占满。
- `--mamba-cache-mode align`：hybrid 模型（Qwen3.6 含 linear+full attention）的 mamba cache 强制对齐布局。
- `--no-enable-prefix-caching`：**关闭 vLLM 自带 GPU 驻留 prefix cache**，确保 hit 测试时 TTFT 降幅只能来自 MemCache external hit，不是 vLLM 本地命中。
- `--prefix-match-unit 128`：GCD(640, 8192)=128，解决 hybrid 模型 attention block_size=640、mamba block_size=8192 时 `group_block_size % hash_block_size == 0` 断言失败。
- `--kv-transfer-config`：`kv_connector=AscendStoreConnector`、`kv_role=kv_both`（同一进程既写又读 KV）、`backend=memcache` 指明走 memcache store 后端。
- 健康检查：`curl http://127.0.0.1:4077/health`，前台模式轮询 60 次（每次 10s，共 600s）并检测 PID 死亡；后台 `--detach` 模式只轮询不检测死亡。

## Step 9 — 运行测试（pytest）

由 `run_test.sh` 调度，根据 `--type` 分发：

```bash
# Layer 2 workload（不起 vLLM，直连 DistributedObjectStore API 测存储延迟）
bash run_test.sh --memcache-dir /path/to/memcache --type workload --device-ids 6,7 --log

# Layer 3 integration（起 vLLM 端到端 TTFT）
bash run_test.sh --memcache-dir /path/to/memcache --type integration --rank 2 \
     --protocol host_shm --device-ids 6,7 --log

# bench（先调 start_service.sh 起 MetaService+vLLM，再跑 simple_throughput_bench.py）
bash run_test.sh --memcache-dir /path/to/memcache --type bench --rank 2 --device-ids 6,7
```

内部最终落到：

```bash
# workload / integration 都是 pytest
python3 -m pytest cases/python/memcache/workload/ -v -s -m workload
python3 -m pytest cases/python/memcache/integration/vllm_integration_test.py -v -s -m rank2

# bench 是 start_service.sh + simple_throughput_bench.py 两轮
bash start_service.sh --device-ids 6,7 --tp 2 --port 4077 ...
python3 cases/python/memcache/integration/simple_throughput_bench.py \
    --host 127.0.0.1 --port 4077 --model Qwen3.6-35B-A3B \
    --num-prompts 20 --request-rate 5 --input-len 1024 --output-len 32 \
    --output-file /tmp/bench_memcache_miss.json
python3 .../simple_throughput_bench.py --cache-test --output-file /tmp/bench_memcache_hit.json
```

- `run_test.sh` 每次测试前自动调 `clean_stale_env.sh`（`--skip-clean` 可跳）。
- 4-rank 加 `--split`：逐个测试分次跑，每个之间清理 NPU + 重启 MetaService，避免 310P3 孤儿 NPU 上下文导致第二个 vLLM OOM。
