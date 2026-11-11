# mooncake 路线：MemFabric + Mooncake MF Transport 测试部署流程

测试用例、TTFT/P50 和常用术语见[测试说明](../../TEST_GUIDE.md)。

本文记录 Mooncake 的历史环境与适配方案。当前目录的测试脚本缺失情况见[入口说明](../README.md#当前目录状态)。

本文档整理 mooncake 路线 vLLM 测试部署的完整流程，按执行顺序拆解每一步的具体命令、命令作用与关键参数含义。

入口是 `scripts/run.sh` 一键全流程，内部按 7 个 step 串联；vLLM 启动逻辑在 `tests/demo_vllm_pd.py` 和 `tests/demo_vllm_ttft.py` 里。

## Step 1 — 拉取三仓源码

由 `run.sh:157-163`（`step_clone`）：

```bash
WORK_DIR=/tmp/test_workdir

git clone --depth 1 -b ljr-dev https://gitcode.com/cnk700i/memfabric_hybrid.git $WORK_DIR/memfabric_hybrid
git clone --depth 1 -b of-dev/v0.3.13.post1 https://gitcode.com/openFuyao/mooncake.git $WORK_DIR/mooncake

# yalantinglibs 用特定 commit（非分支）
git clone https://github.com/alibaba/yalantinglibs.git $WORK_DIR/ylt_src
git -C $WORK_DIR/ylt_src checkout 7801bc9ad9021781f15217552214e325a1cf7373
```

- `--depth 1` 只取最新提交，加速 clone。
- MemFabric 分支 `ljr-dev`（不是 memcache 那边的 `dev-v1.2.0`），Mooncake 分支 `of-dev/v0.3.13.post1`。
- yalantinglibs 是 Mooncake 编译依赖的 header-only 库，必须用指定 commit（否则 API 不兼容）。

## Step 2 — 编译 MemFabric

由 `run.sh:168-198`（`step_build_mf`）：

```bash
cd $WORK_DIR/memfabric_hybrid
git submodule update --recursive --init

bash script/build_and_pack_run.sh \
    --build_mode RELEASE \
    --build_python ON \
    --xpu_type NPU \
    --build_hcom OFF \
    --build_hcom_rdma OFF

# 校验产物
ls output/smem/lib64/libmf_smem.so
```

- `--xpu_type NPU`：目标 NPU（310P3），会编入 ACL/SDMA 路径。
- `--build_python ON`：编 Python 绑定（虽然 mooncake 直接用 .so，但部分测试会 import memfabric_hybrid 验证）。
- `--build_hcom OFF --build_hcom_rdma OFF`：mooncake 走自家 transport 层，不需要 HCOM/RDMA。
- 产物路径：`output/smem/lib64/libmf_smem.so`、`output/hybm/lib64/libmf_hybm_core.so`、`output/aarch64-linux/lib64/`、`output/acc_links/lib64/`、`output/acc_offload/lib64/`。
- `--skip_build_mf` 可跳过，脚本会从 `/home/l30023043/memfabric_hybrid/output` fallback 复用已有产物。

## Step 3 — 打 Mooncake 补丁 + 修正 yalantinglibs 路径

由 `run.sh:203-222`（`step_patch_mooncake`）：

```bash
cd $WORK_DIR/mooncake
git checkout -- . && git clean -fd                    # 幂等：清上次残留
git apply $TEST_ROOT/patches/mooncake/tag_v0.3.13.patch
sed -i "s|SOURCE_DIR /tmp/ylt_src|SOURCE_DIR $WORK_DIR/ylt_src|g" mooncake-common/FindYLT.cmake
```

### 3.1 `tag_v0.3.13.patch`（1909 行，16 个文件）

补丁分两类：**新增 MF transport 实现**（5 个新文件）和**接入 Mooncake 构建/运行时**（11 个修改文件）。

#### 新增文件（MF transport 核心）

| 新文件 | 作用 |
|--------|------|
| `memfabric_smem_dl.h/.cpp` | `MemFabricSmemDl` 单例：dlopen libmf_smem.so + dlsym 加载 28 个 API 指针；对外 `InitSmemBm`（建 store + join rank）、`InitSmemTrans`（建 transfer session）、`GetSegment`（取 DRAM 段） |
| `memfabric_transport.h/.cpp` | `MemFabricTransport : public Transport`：`install()` 按 smemType 调对应 Init；`submitTransfer` 调 `SmemBmCopyBatch` 或 `SmemTransBatchWrite/Read` |
| `CMakeLists.txt` | 编 `mem_fabric_api`（shared）+ `memfabric_mf_transport`（object） |

> **为什么用 dlopen/dlsym**：`USE_MF=OFF` 时 MF 代码不参与编译；`USE_MF=ON` 时运行时按需 dlopen，libmf_smem.so 不在标准路径也能跑。

#### 修改文件（主要改动点）

1. **`CMakeLists.txt`（顶层）+ `FindYLT.cmake`**：pybind11 从 `add_subdirectory(extern/pybind11)` 改为 `find_package`（系统已装）；yalantinglibs 从 github 下载 tarball 改为 `SOURCE_DIR`（本地预 clone）。受限网络下子模块和 FetchContent 下载失败。
2. **`common.cmake`**：新增 `option(USE_MF)`；`USE_MF` 时定义 `USE_MF`+`USE_UB` 宏，include FindUrma，加入 `OPEN_BUILD_PROJECT`。`USE_MF` 宏控制 `#ifdef` 编译；`USE_UB` 复用 kunpeng 的 `ub_allocator`；OPEN_BUILD_PROJECT 开启 NPU 架构检测。
3. **`transfer_engine_py.cpp` + `real_client.cpp`**：分别 `SetSmemTypeFlag(SMEM_TRANS)` 和 `SetSmemTypeFlag(SMEM_BM)`。MooncakeConnector（P2P 传输）对应 `SMEM_TRANS` 子系统；MooncakeStoreConnector（store offload）对应 `SMEM_BM`，两种模式用不同 MemFabric 子系统，必须初始化前设对。
4. **`real_client.cpp`**：`setup_internal` 里调 `MemFabricGetSegment()` 取 DRAM 段，`MountSegment` 挂载后 `return {}` 跳过原 local_buffer 分配。原 Mooncake 自己分配 CPU 内存 + ibv_reg_mr；MF 模式下 SMEM_BM 已通过 HYBM 分配 DRAM 段并注册到 NPU，应复用。
5. **`multi_transport.cpp` + `transfer_engine_impl.cpp`**：工厂注册 `proto == "mf"` → `new MemFabricTransport()`；TransferEngine `init` 时 `#elif defined(USE_MF)` 自动安装 MF 为默认 transport。原只认 tcp/nvlink/ascend 等，`mf` 报 `Unsupported transport`。
6. **`transfer_metadata.cpp`**：`encodeSegmentDesc`/`decodeSegmentDesc` 新增 `protocol == "mf"` 分支。metadata server 持久化/共享 segment 描述符，原只处理 tcp/ascend 等，mf 段解析失败导致跨节点不可用。
7. **CMake 链接**：`mooncake_client` 链接 `ub_allocator`（store 依赖 `ub_allocate_memory` 符号）；`transfer_engine` 链接 `ascendcl memfabric_mf_transport ub_transport Urma::urma`；`transport/CMakeLists.txt` 加 `if(USE_MF)` 构建 mf_transport（复用 kunpeng 的 ub_allocator.cpp）。

### 3.2 `store_mf_protocol.patch`（1 文件，13 行）

`client_service.cpp` 的 `InitTransferEngine` 协议白名单加 `|| protocol == "mf"`。`mf` 原不在白名单，落入 else 分支被当 NIC 协议名报 `Unsupported transport`；加入后走 host memory transport 分支（不需 device name）。

> 注：`run.sh` 的 `_fix_mooncake_store` 用 sed 在线打此补丁（工作区已被 tag_v0.3.13.patch 改过，git apply 可能冲突）。三处冗余覆盖（tag_v0.3.13 / store_mf_protocol / store_mf_host_memory 都含此改动）确保不同应用路径都能生效。

### 3.3 `store_mf_host_memory.patch`（3 文件，132 行）

对 tag_v0.3.13.patch 中 store 部分的增强，解决 DRAM 段重复注册。主要改动点：

1. **`client_service.h/.cpp`**：`MountSegment` / `MountSegmentAndGetId` 新增 `bool skip_register = false` 参数；`skip_register=true` 时跳过 `transfer_engine_->registerLocalMemory()`。SMEM_BM 已通过 HYBM 注册 DRAM 段到 NPU，若 MountSegment 再注册同一段内存会导致地址重叠报错。
2. **`real_client.cpp`**：`MemFabricGetSegment()` 返回 size>0 时 `MountSegment(..., skip_register=true)`；size=0 时 fallback 到原 local_buffer 路径。比 tag_v0.3.13 版本更好：避免重复注册 + 加 fallback 而非硬 return。

## Step 4 — 编译 Mooncake + 组装 Python 包

由 `run.sh:224-261`（`step_build_mooncake`）：

```bash
rm -rf $WORK_DIR/mooncake/build
mkdir -p $WORK_DIR/mooncake/build

cmake -S $WORK_DIR/mooncake -B $WORK_DIR/mooncake/build \
    -DUSE_MF=ON \
    -DWITH_STORE_RUST=OFF \
    -DWITH_P2P_STORE=OFF \
    -DWITH_STORE_GO=OFF \
    -DWITH_EP=OFF \
    -DBUILD_UNIT_TESTS=OFF \
    -DBUILD_BENCHMARK=OFF \
    -DCMAKE_BUILD_TYPE=Release

make -C $WORK_DIR/mooncake/build -j$(nproc)

# 组装 Python 包
mkdir -p $WORK_DIR/mooncake_pkg/mooncake
PY_TAG=$(python3 -c "import sys; print(f'cpython-{sys.version_info.major}{sys.version_info.minor}')")
ARCH_TAG=$(uname -m)
SO_SUFFIX="${PY_TAG}-${ARCH_TAG}-linux-gnu"

cp $WORK_DIR/mooncake/build/mooncake-integration/engine.${SO_SUFFIX}.so \
   $WORK_DIR/mooncake_pkg/mooncake/engine.so
cp $WORK_DIR/mooncake/build/mooncake-integration/store.${SO_SUFFIX}.so \
   $WORK_DIR/mooncake_pkg/mooncake/store.so
touch $WORK_DIR/mooncake_pkg/mooncake/__init__.py
```

- `-DUSE_MF=ON`：启用 MF transport 编译（链接 Step 2 产出的 libmf_smem.so）。
- `-DWITH_STORE_RUST=OFF -DWITH_STORE_GO=OFF`：store 后端只用 C++ 实现（memfabric 接入），不编 Rust/Go 版本。
- `-DWITH_P2P_STORE=OFF -DWITH_EP=OFF`：不需要 P2P store 和 endpoint。
- 产物 `engine.so`（MooncakeConnector 用）和 `store.so`（MooncakeStoreConnector 用）按 cpython 版本+架构命名，统一拷成不带后缀的 `engine.so` / `store.so` 供 PYTHONPATH 导入。

## Step 5 — 运行 10 个 PD 传输测试（双进程，验证底层 MF API）

由 `run.sh:266-300`（`step_test`）：

```bash
export MEMFABRIC_OUTPUT=$WORK_DIR/memfabric_hybrid/output
export MOONCAKE_BUILD=$WORK_DIR/mooncake/build
export PYTHONPATH=$WORK_DIR/mooncake_pkg:${PYTHONPATH:-}
export GLOG_logtostderr=1
export GLOG_minloglevel=2
export MF_STORE_URL=tcp://127.0.0.1:8570
export MF_DRAM_SIZE=$((1 << 30))    # 1GB
export MF_HBM_SIZE=0
export MF_OP_TYPE=device_sdma
export MF_PD_OP_TYPE=device_sdma
export MF_LOG_LEVEL=1

export LD_LIBRARY_PATH="$MEMFABRIC_OUTPUT/aarch64-linux/lib64:$MEMFABRIC_OUTPUT/smem/lib64:$MEMFABRIC_OUTPUT/hybm/lib64:$MEMFABRIC_OUTPUT/acc_links/lib64:$MEMFABRIC_OUTPUT/acc_offload/lib64:$MOONCAKE_BUILD/mooncake-common:$MOONCAKE_BUILD/mooncake-transfer-engine/src/transport/mf_transport:${LD_LIBRARY_PATH:-}"

python3 -u tests/demo_pd_transfer.py            # 全部 10 个
# 或单个
python3 -u tests/demo_pd_transfer.py single_write
```

- 每个测试启动两个 Python 进程：Sender（`MF_SMEM_TRANS_ROLE=Prefill`, rank 0）+ Receiver（`MF_SMEM_TRANS_ROLE=Decode`, rank 1），通过 `/tmp/mf_pd/` 文件协调握手，`numpy.array_equal` 校验数据。
- `MF_HBM_SIZE=0`：PD 传输测试只用 DRAM，不占 HBM，便于和 vLLM 测试共用设备。
- 10 个场景覆盖：单/多缓冲区同步读写、异步读写+轮询、大缓冲区(1MB)、多段注册、probe 连通性、register/unregister 生命周期。

## Step 6 — vLLM 集成测试前自动打 vLLM 运行时补丁

由 `run.sh:324-388`（`step_vllm_test`）在跑 vLLM 测试前调三个内部函数，**直接改已安装的 vLLM Python 文件**（不 rebuild vLLM）。所有 patch 幂等：先 grep marker 字符串，已存在就跳过。主要改动点：

1. **`utils.py` `get_transfer_cache_regions`**（hybrid KV cache tuple）：MambaSpec 分支后插入 `if isinstance(cache, tuple): return list(cache)`。Qwen3.5 hybrid 模型 full attention 层传 `(k,v)` tuple（非 MambaSpec），原代码访问 `tuple.shape` 抛 `AttributeError`。
2. **`store/worker.py` `_repr_tensor`**（同类 tuple bug）：`isinstance(v, torch.Tensor | list)` → 加 `tuple`。hybrid 模型 KV cache 是 tuple 时只认 list 抛 `AssertionError`。
3. **`client_service.cpp` 协议白名单**（sed 在线打）：加 `|| protocol == "mf"`。同 3.2 store_mf_protocol.patch；sed 而非 git apply 是因为工作区已被 tag_v0.3.13.patch 改过。改完 `make store` 重编 store.so。
4. **编译 `libub_allocator.so`**：`g++ -shared` 把 `ub_allocator.cpp.o` 编成独立 .so。store.so 引用 `ub_allocate_memory`/`ub_free_memory` 符号但 CMake 没编独立 .so（只在 ub_transport object lib 里），运行时 `undefined symbol`。供 `LD_PRELOAD` 使用。
5. **`store/worker.py` `register_kv_caches` 按组分配段地址**：所有 layer 段地址混一个列表 → 按 `self._kv_cache_groups` 分组，每个 `token_db` 只拿自己组地址。hybrid 模型有多组 KV cache group（linear/full attention 各一组），block_size 和布局不同，原混在一起导致 token_db 拿到别组地址，SDMA 越界。
6. **`store/worker.py` `save_put` / `load_get` SDMA 传输重试**：`batch_put_from_multi_buffers` / `batch_get_into_multi_buffers` 失败后直接标记 partial_failure → 加 2 次重试（间隔 50ms/100ms 递增）。310P3 SDMA 首次传输可能失败（HBM access timing：NPU compute 刚结束、HBM 还在刷新，SDMA 立刻读会失败），原首次失败 KV 没写入 store，后续 hit 变 miss。

## Step 7 — 启动 vLLM（4 种模式，命令各不同）

`demo_vllm_pd.py:62-127` 构造命令，`build_env` 注入 MF 库路径和 bootstrap 端口。4 个测试场景的启动命令分别如下。

### 7.1 vllm_baseline（无 KV transfer，验证模型能加载推理）

```bash
ASCEND_RT_VISIBLE_DEVICES=0 \
python3 -m vllm.entrypoints.openai.api_server \
    --model /home/models/Qwen3.5-2B-W8A8-310P \
    --host 127.0.0.1 --port 8010 \
    --enforce-eager --max-model-len 512 --max-num-seqs 4 \
    --max-num-batched-tokens 512 --trust-remote-code \
    --gpu-memory-utilization 0.85 --seed 1024 --dtype float16 \
    --served-model-name qwen35-2b
# 不带 --kv-transfer-config
```

### 7.2 vllm_kvboth_mf（MooncakeConnector，单进程既写又读）

```bash
ASCEND_RT_VISIBLE_DEVICES=0 \
PYTHONPATH=$WORK_DIR/mooncake_pkg:$PYTHONPATH \
LD_LIBRARY_PATH=<MF + mooncake build 路径>:$LD_LIBRARY_PATH \
MF_STORE_URL=tcp://127.0.0.1:8570 \
MF_DRAM_SIZE=1073741824 MF_HBM_SIZE=0 \
MF_OP_TYPE=device_sdma MF_PD_OP_TYPE=device_sdma \
VLLM_MOONCAKE_BOOTSTRAP_PORT=8998 \
python3 -m vllm.entrypoints.openai.api_server \
    --model /home/models/Qwen3.5-2B-W8A8-310P ... \
    --kv-transfer-config '{"kv_connector":"MooncakeConnector","kv_role":"kv_both","kv_connector_extra_config":{"mooncake_protocol":"mf"}}'
```

- `mooncake_protocol=mf` 让 MooncakeConnector 的 TransferEngine 用 MF transport（调 MemFabric SDMA），不走 TCP/rdma。
- bootstrap port（8998）是 Mooncake 自家的 P2P 发现端口，和 vLLM 的 8010 分开。

### 7.3 vllm_pd_disagg_mf（PD 分离，prefiller + decoder + proxy 三进程）

310P3 不支持跨芯片 PeerAccess，prefiller 和 decoder 共用同一设备，各分 0.5 显存：

```bash
# prefiller (kv_producer)
ASCEND_RT_VISIBLE_DEVICES=0 MF_SMEM_TRANS_ROLE=Prefill \
VLLM_MOONCAKE_BOOTSTRAP_PORT=8998 VLLM_PORT=8011 \
python3 -m vllm.entrypoints.openai.api_server \
    --model ... --port 8010 --gpu-memory-utilization 0.5 \
    --max-model-len 256 --max-num-seqs 2 \
    --kv-transfer-config '{"kv_connector":"MooncakeConnector","kv_role":"kv_producer","kv_connector_extra_config":{"mooncake_protocol":"mf"}}' &

# decoder (kv_consumer)
ASCEND_RT_VISIBLE_DEVICES=0 MF_SMEM_TRANS_ROLE=Decode \
VLLM_MOONCAKE_BOOTSTRAP_PORT=8998 VLLM_PORT=8021 \
python3 -m vllm.entrypoints.openai.api_server \
    --model ... --port 8020 --gpu-memory-utilization 0.5 \
    --max-model-len 256 --max-num-seqs 2 \
    --kv-transfer-config '{"kv_connector":"MooncakeConnector","kv_role":"kv_consumer","kv_connector_extra_config":{"mooncake_protocol":"mf"}}' &

# proxy（vLLM 自带示例脚本，路由请求到 prefiller/decoder）
python3 /vllm-workspace/vllm/examples/disaggregated/mooncake_connector/mooncake_connector_proxy.py \
    --host 127.0.0.1 --port 9010 \
    --prefill http://127.0.0.1:8010 8998 \
    --decode  http://127.0.0.1:8020
```

- **必须同时启动 prefiller 和 decoder**：MemFabric topology preflight 需要所有 rank 参与，prefiller 先注册完、decoder 后到会超时（5s gather timeout）。
- 请求发到 proxy，proxy 路由到 prefiller 做完 prefill，KV 通过 MemFabric SDMA 传到 decoder，decoder 接力 decode 返回。

### 7.4 vllm_kv_offload_mf（MooncakeStoreConnector + mooncake_master，KV offload 到分布式存储）

先起 mooncake_master（元数据服务，类似 memcache 的 mmc_meta_service）：

```bash
# 起 master
LD_LIBRARY_PATH=<MF + mooncake store 路径>:$LD_LIBRARY_PATH \
$WORK_DIR/mooncake/build/mooncake-store/src/mooncake_master --port=50051 --logtostderr=1 &

# 生成 mooncake 配置 JSON
cat > /tmp/vllm_test_logs/mooncake_config.json <<'EOF'
{
  "mode": "embedded",
  "metadata_server": "P2PHANDSHAKE",
  "master_server_address": "127.0.0.1:50051",
  "global_segment_size": "2GB",
  "local_buffer_size": "1GB",
  "protocol": "mf",
  "device_name": "",
  "enable_offload": false
}
EOF

# 起 vLLM（关键：LD_PRELOAD libub_allocator.so，否则 store.so 有 undefined symbol）
ASCEND_RT_VISIBLE_DEVICES=0 \
MOONCAKE_CONFIG_PATH=/tmp/vllm_test_logs/mooncake_config.json \
MF_SMEM_TYPE=bm MF_SMEM_TRANS_ROLE=Prefill \
VLLM_SERVER_DEV_MODE=1 \
VLLM_LOGGING_LEVEL=DEBUG \
VLLM_MOONCAKE_STORE_TIER_LOG=1 \
LD_PRELOAD=$WORK_DIR/mooncake/build/.../libub_allocator.so \
python3 -m vllm.entrypoints.openai.api_server \
    --model ... --port 8010 --gpu-memory-utilization 0.85 \
    --max-model-len 2048 --max-num-seqs 4 \
    --enable-prefix-caching \
    --kv-transfer-config '{"kv_connector":"MooncakeStoreConnector","kv_role":"kv_both"}'
```

关键参数答疑：

- `MooncakeStoreConnector`（不是 `MooncakeConnector`）：走 store 模式，KV cache offload 到分布式存储池，不是 P2P 直传。
- `MOONCAKE_CONFIG_PATH`：指向上面写的 JSON，`protocol=mf` + `master_server_address` 指向刚起的 master。
- `MF_SMEM_TYPE=bm`：让 HYBM 用 SMEM_BM 模式分配 DRAM 段，store 才能用 HYBM 管理的 CPU 内存池做 KV offload。
- `LD_PRELOAD libub_allocator.so`：store.so 引用了 `ub_allocate_memory` / `ub_free_memory` 符号但没链接，必须 preload 这个 .so。
- `VLLM_SERVER_DEV_MODE=1`：启用 `POST /reset_prefix_cache` dev API，第二次请求前清 vLLM 本地缓存（不清 store），强制走 store hit。
- `--enable-prefix-caching`：这里和 memcache 路线相反，必须**开**，因为 310P patch 会把 mamba_block_size 钉成 640，scheduler_block_size=640，prompt >= 640 token 才能填满一个 block 触发 save_put；不开 prefix caching 时 mamba_block_size 默认 max_model_len，会破坏 MooncakeStoreConnector 校验。
- 测试发两次请求：请求 1 写 KV 到 store，sleep 10s 等异步 save_put，`POST /reset_prefix_cache` 清本地，请求 2 用相同前缀+短后缀，验证从 store 命中。

## Step 8 — 运行 TTFT 基准测试（streaming 量化首 token 延迟）

由 `run.sh:390-449`（`step_ttft_test`）：

```bash
python3 -u tests/demo_vllm_ttft.py
# 或单场景
python3 -u tests/demo_vllm_ttft.py ttft_baseline
python3 -u tests/demo_vllm_ttft.py ttft_store_hit
python3 -u tests/demo_vllm_ttft.py ttft_offload
```

三个场景的 vLLM 启动命令复用 Step 7 的模式，区别在请求侧：

| 场景 | vLLM 启动配置 | 请求侧动作 |
|------|--------------|-----------|
| `ttft_baseline` | 无 KV transfer、无 prefix caching | 10 次 unique prompt，统计 P50/P90/P99 |
| `ttft_store_hit` | MooncakeStoreConnector + prefix caching + mooncake_master | Phase 1 miss（unique prompt 写 store）-> sleep 10s -> reset 本地 -> Phase 2 同 prompt 从 store 读，100% 重叠 |
| `ttft_offload` | 同 store_hit 配置 | Phase 1 miss（768-token base）-> Phase 2 base+64-token suffix，复用 base 中已保存的前缀，计算其余部分与新增 suffix |

- 用 `POST /v1/completions` + `stream: true`，度量请求发送到首 token 的时间。
- 规模刻意小（`TTFT_WARMUP=2` + `TTFT_MEASURED=10`，`TTFT_PROMPT_TOKENS=768`），整套 ~15-20 分钟。
- 隔离方式：`VLLM_SERVER_DEV_MODE=1` + `POST /reset_prefix_cache` 清本地 block cache（不清 store），对比 miss P50 vs hit P50 验证 Mooncake store 命中降幅。
