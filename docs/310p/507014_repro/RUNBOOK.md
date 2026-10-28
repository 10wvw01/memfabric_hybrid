# Gate E 507014 复现与取证手册（RUNBOOK）

> 2026-10-26 · w00930648 容器实机验证通过
> 结论：已在 dev6/7 首次复现 507014（smoke4 会话，第 1232 代 V2 融合算子），
> 并将丢失点定位到 SQE 级（② 到达戳未落地 / ③ 完成戳已落地），
> 机制定位为「AICPU 跨 die 写对端邮件像 → 本端 SDMA 立刻跨 die 回读」的可见性竞态
> + 邮件槽[1023] 与 arrHead 共享 64B 缓存行的结构性边界效应。

## 快速指引（TL;DR）

- **需要什么环境**：Atlas 300I Duo（310P3）一张卡双 die 空闲 + CANN 9.1.0 + bisheng + pip 镜像可达（详见 §0）；
- **怎么操作**：§1 携带资产 → §2-4 环境装配（约 40 分钟）→ §5 冒烟 → 正式 620s 会话 → §6 命中后按"四问判别流程"取证；
- **目录约定**：全文命令以 `$WORK_ROOT` 为工作根目录（源机为 `/tmp/opencode`，移植时自定，如 `~/repro`）；
- **本手册配套**：`tools/`（会话运行器/邮箱猎手/探针）+ `evidence/smoke4_first_fault/`（首次命中原始证据）+ `assets_manifest.txt`（外置资产 SHA 清单）。

---

## 0. 机器与资源前提

| 项 | 要求 |
|---|---|
| 硬件 | Atlas 300I Duo（Ascend310P3，dav-2002），一张卡两个 die 空闲（>40GB 内存、大页可用） |
| 驱动/CANN | driver 26.1.1 / cann-9.1.0（`ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0`） |
| 编译器 | bisheng（CANN 自带，`command -v bisheng`） |
| Python | 3.12.13 |
| 网络 | 可达 pip 镜像（华为云源可用） |
| 关键软件版本 | torch 2.10.0+cpu / torch_npu 2.10.0.post4 / vllm 0.27.1 / triton 3.6.0 |

**避坑（实机血泪）**：
- ❌ 不要在 die 对上调 `aclrtDeviceEnablePeerAccess`（同卡 die 对必返 507899 且可能触发驱动级死锁，见仓库 `310p-capability_matrix/README.md`）；
- ❌ npu-smi 全局命令在任一卡故障时会 D 态挂死——一律用单卡查询并带 timeout：`timeout 15 npu-smi info -t usages -i <卡ID> -c <chip>`；
- ❌ 共享机上先确认目标卡双 die 空闲（别的容器任务会动态抢内存导致 serve 启动失败 "Free memory ... less than desired"）。

---

## 1. 需要携带的资产（含 SHA 校验）

从源机器（本机）拷贝以下内容（打包 `$WORK_ROOT/repro/` + 两个 git 仓库即可）：

| 资产 | 来源路径 | SHA256（校验用） |
|---|---|---|
| memfabric 源码 | git 仓库，分支 `diag/310p3-g1d-minimal-forensics-20261021`（HEAD `54d6376d`） | — |
| SetAccess 物理号修复 | 同仓库 `fix/310p-setaccess-physical-devid-20261023` 分支的提交 `5802f550` | — |
| vllm-ascend 源码 | git 仓库，同 diag 分支（HEAD `46260392a`） | — |
| **V2 设备库**（D1R4 原版） | 源机 `/home/w00804146/310P/vllm-ascend-g1d/vllm_ascend/libmf310p_v2_trial_device_g1d.so` | `d9a6de66…bfa73` |
| **vllm 插件**（D1R4 原版） | 同目录 `vllm_ascend_C.cpython-312-aarch64-linux-gnu.so` | `bcbb4f67…97550` |
| libmf310p_device.so | 同目录 | — |
| **自定义算子包**（gitignore 产物，新克隆没有！） | 源机 `/home/w00804146/310P/vllm-ascend-g1d/vllm_ascend/_cann_ops_custom/` 整目录 | — |
| 生产版 memfabric python 包 | 源机 `/home/h00949337/vllm-venv/.../site-packages/memfabric_hybrid/`（1.2.1） | — |
| 模型 | `Qwen3.6-35B-A3B-w8a8`（38G，10 分片+tokenizer） | — |

**SHA 快速校验**（v12 launch json 应逐字节一致——diag 分支可复现构建的直接证据）：
```
libmf_sdma_orch_v12.json → 90172726965f4944ed955237c296faf1622762897f742ffbecf69a9e2fb554d0
```

---

## 2. Python 环境（venv 装配，~30 分钟）

```bash
# 2.1 建 venv
python3 -m venv $WORK_ROOT/venv-repro

# 2.2 复用源机现成 torch 栈（symlink 包目录, 不链 dist-info——防 pip 误删源机文件）
SP=$WORK_ROOT/venv-repro/lib/python3.12/site-packages
SRC=<源机 venv site-packages>
cd $SRC && for f in *; do
  case $f in vllm*|memfabric*|__editable__*|__pycache__) continue;; esac
  ln -sfn "$SRC/$f" "$SP/$f"
done

# 2.3 装 vllm（--no-deps：依赖已由 symlink 覆盖）
$WORK_ROOT/venv-repro/bin/pip install --no-deps vllm==0.27.1

# 2.4 换 triton（vllm 0.27.1 的 gluon 导入需要新 API, 旧 3.5.0 会 ImportError）
rm -f $SP/triton $SP/triton-3.5.0.dist-info
$WORK_ROOT/venv-repro/bin/pip install triton==3.6.0

# 2.5 装生产版 memfabric python 包 + 热替换 diag 构建的 Host SO（见 §4 产物）
cp -rL $SRC/memfabric_hybrid $SP/
cp -rL $SRC/memfabric_hybrid-1.2.1.dist-info $SP/ 2>/dev/null || true

# 2.6 手工构造 vllm-ascend 的 dist-info（等效 pip install -e 的 entry points, 免重编译 C）
D=$SP/vllm_ascend-0.27.1.diag.dist-info && mkdir -p $D
printf 'Metadata-Version: 2.1\nName: vllm-ascend\nVersion: 0.27.1.diag\n' > $D/METADATA
cat > $D/entry_points.txt << 'EOF'
[vllm.platform_plugins]
ascend = vllm_ascend:register

[vllm.general_plugins]
ascend_kv_connector = vllm_ascend:register_connector
ascend_model_loader = vllm_ascend:register_model_loader
ascend_service_profiling = vllm_ascend:register_service_profiling
ascend_model = vllm_ascend:register_model
EOF
touch $D/RECORD
```

**坑位对照表**（症状 → 根因 → 修复）：

| 症状 | 根因 | 修复 |
|---|---|---|
| `ImportError: cannot import name 'constexpr_type' from triton` | triton 3.5.0 缺新 API | §2.4 换 triton 3.6.0 |
| `RuntimeError: Failed to infer device type` | vllm 找不到平台插件 | §2.6 补 platform_plugins 入口 |
| `ModuleNotFoundError: vllm.model_executor.layers.fla` | FLA 别名未装（4 个 general_plugins 入口负责早期打补丁） | §2.6 补齐 general_plugins |
| `ImportError: cannot import name '_build_info'` | 构建期生成文件缺失 | §3.3 手工生成 |
| `Insufficient space in /dev/shm: 160 MiB required` | 容器默认 64M | `mount -o remount,size=2G /dev/shm` |
| `RuntimeError: aclnnCausalConv1dV310...` | 自定义算子包缺失（gitignore 产物） | §1 拷贝 `_cann_ops_custom` |

---

## 3. MemFabric diag 构建（~3 分钟）

```bash
# 3.1 克隆 diag 分支 + cherry-pick 物理号修复
git clone <源>/memfabric_hybrid $WORK_ROOT/mf_diag -b diag/310p3-g1d-minimal-forensics-20261021
git -C $WORK_ROOT/mf_diag cherry-pick 5802f550   # SetAccess 逻辑号→物理号

# 3.2 构建（⚠ 必须显式 -S 指定源码树!）
cmake -S $WORK_ROOT/mf_diag -B $WORK_ROOT/mf_diag/build \
    -DXPU_TYPE=NPU -DBUILD_PYTHON=ON -DBUILD_UT=OFF -DBUILD_TEST=OFF \
    -DMF_SDMA_ORCH_KVER=12 -DMF_SDMA_ORCH_G1C_DEVICE_TRACE=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build $WORK_ROOT/mf_diag/build -j32
```

**⚠ 头号坑（本次最大事故）**：cmake 不带 `-S` 时用 shell 当前目录当源码树——若 shell 停在别的 worktree，会**静默编错分支**（20 秒"成功"，产物无修复）。双重验证：

```bash
# 验证 1: 编译源路径正确
grep -m1 "mf_diag/src" $WORK_ROOT/mf_diag/build/src/hybm/csrc/CMakeFiles/hybmm_objects.dir/DependInfo.cmake
# 验证 2: 机器码含物理号转换（应看到 HybmPhysicalDeviceIdOf → AclrtMemSetAccess 调用链）
objdump -d -C --disassemble="ock::mf::MetaSetDeviceAccess(unsigned short, unsigned long)" \
    $WORK_ROOT/mf_diag/build/src/hybm/csrc/libmf_hybm_core.so | grep -E "bl" | head -3
# 验证 3: v12 json SHA == 90172726...
sha256sum $WORK_ROOT/mf_diag/build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_v12.json
```

```bash
# 3.3 生成 _build_info.py（vllm_ascend 需要的构建期文件）
printf '# Auto-generated\n__device_type__ = "_310P"\n' > $WORK_ROOT/vllm-ascend-diag/vllm_ascend/_build_info.py

# 3.4 拷入 D1R4 原版二进制 + 算子包（见 §1 资产表, SHA 校验后）
cp <资产>/vllm_ascend_C.cpython-312-aarch64-linux-gnu.so $WORK_ROOT/vllm-ascend-diag/vllm_ascend/
cp <资产>/libmf310p_v2_trial_device_g1d.so $WORK_ROOT/vllm-ascend-diag/vllm_ascend/
cp <资产>/libmf310p_device.so $WORK_ROOT/vllm-ascend-diag/vllm_ascend/
cp -r <资产>/_cann_ops_custom/vendors $WORK_ROOT/vllm-ascend-diag/vllm_ascend/_cann_ops_custom/

# 3.5 部署工具构建
make -C $WORK_ROOT/mf_diag/tools/aicpu_kernel_deploy ASCEND_HOME_PATH=$ASCEND_HOME_PATH
```

## 4. 运行时安装布局（双位置 Host SO + kernel 三件套）

```bash
MF=/usr/local/memfabric_hybrid/latest
mkdir -p $MF/aarch64-linux/lib64 $MF/aarch64-linux/hybm/aicpu_kernel
B=$WORK_ROOT/mf_diag/build
cp $B/src/hybm/csrc/libmf_hybm_core.so   $MF/aarch64-linux/lib64/                      # 位置1
cp $B/src/hybm/csrc/libmf_hybm_core.so   $SP/memfabric_hybrid/lib/libmf_hybm_core.so   # 位置2(包内)
cp $B/src/smem/csrc/libmf_smem.so        $MF/aarch64-linux/lib64/
cp $WORK_ROOT/mf_diag/tools/aicpu_kernel_deploy/orch_deploy_helper $MF/aarch64-linux/lib64/
cp $B/src/hybm/ops/hybm_kernel/{libmf_sdma_orch.so,libmf_sdma_orch.json,libmf_sdma_orch_v12.json} \
   $MF/aarch64-linux/hybm/aicpu_kernel/
# 双位置 SHA 必须一致（g1d_run.sh 的门禁会校验）
sha256sum $MF/aarch64-linux/lib64/libmf_hybm_core.so $SP/memfabric_hybrid/lib/libmf_hybm_core.so

# set_env.sh（⚠ 变量名勿用 MF_PREFIX——会跟会话脚本撞名, 本次踩过）
cat > /usr/local/memfabric_hybrid/set_env.sh << 'EOF'
#!/usr/bin/env bash
export MF_RUNTIME_LIB=/usr/local/memfabric_hybrid/latest/aarch64-linux/lib64
export LD_LIBRARY_PATH="$MF_RUNTIME_LIB:$LD_LIBRARY_PATH"
EOF

# G1B 日志注入器（sitecustomize——把 hybm 日志提到 INFO 级, 否则 [MF_DIAG] 全被过滤）
mkdir -p $WORK_ROOT/pyinj
sed 's|_LIB = "<源机路径>.../memfabric_hybrid/lib/libmf_hybm_core.so"|_LIB = "'$SP'/memfabric_hybrid/lib/libmf_hybm_core.so"|' \
    <vllm-ascend-diag>/csrc/_310P/memfabric_mm_ar_v2/test/g3_e2e_scripts/g1b_sitecustomize.py \
    > $WORK_ROOT/pyinj/sitecustomize.py
```

## 5. 会话运行器（run_gate_e.sh 要点）

完整脚本见 `$WORK_ROOT/repro/run_gate_e.sh`（已含全部细节）。核心结构：

```bash
# 环境块（与 g1d_run.sh 等价, 设备/端口/模型路径按目标机改）
ASCEND_RT_VISIBLE_DEVICES=6,7          # 目标空闲卡（非 0,1 时必须已完成 §3.1 物理号修复）
MF_SDMA_ORCH_JSON=$MF/.../libmf_sdma_orch_v12.json
MF_SDMA_ORCH_DIAG=1  MF_SDMA_ORCH_G1C_TRACE=1  MF_SDMA_ORCH_G1D_TRACE=1
MF310P_V2_G1C_TRACE=1  MF310P_V2_G1D_TRACE=1
PYTHONPATH=$WORK_ROOT/pyinj:$WORK_ROOT/vllm-ascend-diag
VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR=1
VLLM_ASCEND_310P_MEMFABRIC_STORE_URL=tcp://127.0.0.1:18581
VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES=$((96*1024*1024))
VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT=1
VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK=1
VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE=1
VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS=1024
VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS=4

# ① serve 前预部署 v12 kernel（独立进程走 KFC, 规避 torch_npu 进程内 KFC 错误闩锁）
ASCEND_RT_VISIBLE_DEVICES=6 $MF/lib64/orch_deploy_helper 0 $MF_SDMA_ORCH_JSON
#   rc=0 全新部署 / rc=3 已存在——两者都可继续

# ② serve（变体A=原故障配置 / 变体B=同事新参考: --compilation-config 图模式+output 2048）
vllm serve $MODEL --enforce-eager ... &

# ③ 健康检查: "Enable 310P3 TP=2 MemFabric unquantized mm_ar" 必须 == 80 层
#    "[G1B] sitecustomize: hybm_set_log_level(1) rc=0" 必须出现（否则 [MF_DIAG] 缺失）

# ④ 滚动负载: vllm bench serve --random-input-len 4096 --random-output-len 256 \
#    --num-prompts 25 --max-concurrency 10 (滚动至 620s)

# ⑤ 507014 watchdog —— ⚠ 必须盯到 serve 完全退出（含停机期! smoke4 的故障就发生在
#    末代 kernel 停机窗口, 旧版只盯负载期会漏判）
```

**冒烟验证顺序**（建议照此渐进）：
1. （可选）hunter 短跑：纯邮箱链路验证（建池/物理号修复/kernel 部署一次全过）；
2. serve 冒烟 300s：健康 + 首批请求存活（CausalConv1d 等算子问题在此暴露）；
3. 正式 620s 会话。

## 6. 命中 507014 后的取证判别（本手册的核心价值）

从 `serve.log` 提取四类帧，按序回答四个问题：

```bash
L=<session>/serve.log
grep "\[V2_G1C\]" $L | tail -4          # 每核 stage
grep "\[MF_DIAG\] tick" $L | tail -4    # 双端请求环游标
grep "\[MF_G1D\]" $L | tail -4          # 到达环被等槽实测值
grep "\[V2_G1D\]" $L | tail -4          # wait 返回捕获
```

**判别流程（三分类 → SQE 级）**：

| 步骤 | 看什么 | 结论分支 |
|---|---|---|
| ① 信号发了吗 | 对端 `reqTail` 是否 > 被等件号 | 否 → 调度停滞类（D1R4 型, 非丢失） |
| ② 落了吗 | 被等槽 `headStamp` == 期待值？ | 是 → 不是这个故障 |
| ③ 迟到吗 | 后继邮件已落 + 冻结帧持续数十秒 | 满足 → 真丢失 |
| ④ 丢在哪 | `quiet[seq%64]`（SQE③回执）vs `stamp[被等槽]`（SQE②签到） | **quiet=新值 而 stamp=旧值 → SQE 级丢失实锤** |
| ⑤ 边界指纹 | 被等槽号==1023？旧值==期待值−1024？ | 双满足 → 跨 die 邮件像可见性竞态 + arrHead 共行 |

**实测样本（smoke4）**：gen 1232，rank0 等 chunk1（stamp 6144，槽 1023），`quiet63=6144` ✓ / `stamp[1023]=5120` ✗（6144−1024=5120 恰差一圈），后继 6145/6146 正常落环——四问全部命中"真丢失"。

## 7. 根因与修复方向（已定位, 未动代码）

- **设计缺陷**：`issueMailSqes` 的 ②③ 抄写源 = 对端邮件像（`arrMailSlot`，在 A 芯片）——跨 die 读自己刚跨 die 写的值，无可见性保证；
- **结构放大器**：邮件槽[1023]（0xAA00）与 `arrHead`（0xAA20）同 64B 缓存行——全环唯一与"对端热变量"共行的格子，B 的远端写与 A 的 arrHead 写互相踩；
- **修复 A（几行）**：AICPU 把 `seq+1` 写入**本端**暂存槽，②③ 源地址改指本地（跨 die 只剩写，与从未失败的 SQE① 数据搬运同构）；
- **修复 B（布局）**：邮件数组与 `arrHead` 之间垫 64B 填充，隔离缓存行（需 bump KVER 重部署）。

## 8. 复现记录（源机）

| 会话 | 配置 | 结果 |
|---|---|---|
| smoke4 | 300s, eager, dev6/7 | **FAULT**：gen 1232 双 rank 507014（rank1 首爆 aclnnSwiGlu 同步点，距冻结 68s=看门狗节拍）；原始证据已随本手册入库：`evidence/smoke4_first_fault/`（serve.log 含 824 帧 MF_DIAG / 728 帧 V2_G1C / 冻结 G1D 帧，provenance 含全部二进制 SHA） |
| gateA1 | 620s, eager, dev6/7 | **CLEAN**：3 轮 bench、gen 3008、15027 封/方向、507014=0；因运行器早期版本停机死锁 bug，serve 空转 ~48h——11631 个 epoch 干净轮转、零自发故障（免费长测）；日志 654MB 留源机未入库 |

> 统计口径提醒：以**邮件件数**而非会话数为进度度量；smoke4 命中时约 1.5 万件/方向。
> 修复侧对照：三根因修复已合入 `dev-v1.2.0`（布局 V3 独占缓存行 + RS_STAGE 本端暂存 + SQ 链加固，KVER 28），本手册环境可作"修复前基线 vs 修复后"验证闭环的基线侧。
