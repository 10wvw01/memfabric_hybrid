# 08_310p_aicore_aicpu_sdma：310P Duo 单卡双 die 2-rank 设备发起 SDMA（AICore×AICPU 协同）

## 场景

Atlas 300I Duo（Ascend310P3）单卡双 die（dev0/dev1）2-rank SHM 对称内存池上，
演示**设备发起的计算-通信协同**（与 07 的 host 编排互补）：

- **AICore kernel 产数据**（写池 GVA + 逐行 clean）→ `smem_shm_sdma_signal` 投递搬运
- **AICPU epoch 编排 kernel 常驻消费**（监督线程 ~22s 自限续命，watchdog 30s 实证约束）：
  消费段尾保留区请求环 → SQE 三连（数据 + 到达戳 + 完成戳，批末 doorbell，FIFO 保序）
- **对端 AICore `smem_shm_sdma_wait` 按序收件**（邮件 {dst,len,imm} 自描述落点 → 读改写 INC →
  回程 signal）；源缓冲复用前 `smem_shm_sdma_quiet`
- **host 零数据面**：仅建池/起 kernel/收结果（`smem_shm_sdma_submit/wait/get_result` 已废止，
  设备侧原语见 `smem_shm_aicore_base_sdma.h`）

全内存通知链（双环邮箱/戳 SQE 对），段尾保留区 48KB 由 memfabric 全托管。

## 目标

- 验证 SHM API 设备发起语义在 310P 落地：`smem_shm_create(SDMA)` → 传输准备
  （CP 流 + 工作区 + 编排 kernel）全自动完成
- 验证 AICore 直接 GM 访问 AclVmm 池 GVA（产/读改写/dcci 轮询标志）
- **两阶段验收语义**：
  - ① 多 chunk 流水（8×128KB）——功能正确性（1MB 逐字校验）
  - ② 串行 ×8（chunks=1 同步逐次）——无重叠对照
  - 输出**通信被计算掩盖**的加速比量化（流水稳态 ≈ AICore 单 chunk 计算时间）

## 使用能力（设备侧自驱）

`smem_shm_create`（SDMA 位，310P 上协同自动启用）→ `smem_shm_sdma_get_workspace`
（就绪判定）→ AICore kernel 内 `smem_shm_sdma_signal/`wait`/`quiet`（设备头原语，
gva 首参；高频路径 `smem_shm_sdma_reserved(gva)` 预计算 + `_at` 变体）→
`smem_shm_destroy`。host 无数据面调用。

## 规模建议

- world_size=2 固定（同卡 die 对映射 dev0/dev1）
- 单次搬运 ≤ 1MB（AICPU SQE 上限）；len 须 4 字节对齐
- 请求环深 64（满环 signal 背压自旋）；到达环深 1024（消费须及时，溢出 wait 显式报错）
- 每 rank 段末尾 48KB 为邮箱保留区（memfabric 托管，勿存放用户数据）

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1
- MPI（双 rank 拉起与栅栏；本机两套可用，**编译与启动必须同一套**，混用会段错误：
  发行版 4.1.2：`/usr/bin/mpirun` + 头/库 `/usr/lib/aarch64-linux-gnu/openmpi/{include,lib}`
  （编译需 `-DOMPI_SKIP_MPICXX`）；自装 4.1.5：`/usr/local/openmpi` 全套）
- 协同编排 kernel 三件套（`libmf_sdma_orch.so` + `libmf_sdma_orch.json` +
  `libmf_sdma_orch_v7.json`）位于 launch json 同目录（构建目录或安装前缀）——
  **初始化自动部署**：设备侧缺失时库内自动完成 CUST 迁移落盘 + CP1 搜索路径就位 +
  完整性校验（符号+尺寸），无需手工部署；手动/诊断部署链见
  `tools/aicpu_kernel_deploy/`（预部署与通道排查用）
- 环境变量（均可选）：`MF_SDMA_ORCH_JSON` 显式指定 launch json（缺省依次自动发现：
  安装前缀 → CWD）；`MF_SDMA_ORCH_DISABLE=1` 显式禁用协同（仅诊断用）
  （json 由主构建 `mf_sdma_orch_kernel` 目标产出）

## 验收标准

1. ①流水：8 chunk 全量（1MB）逐字校验 mismatch=0，双侧 kernel mainRet=0、AICore
   状态走满（0xD2/0xE1）、headAfter 采样前进（TSFW 消费证据快照，kernel 尾部不等待全部消费）
2. **掩盖量化**：①流水总时延 < ②串行总时延（输出加速比；串行基线含逐轮 launch 与
   轮间会合开销，加速比度量的是"批量流水 vs 逐轮往返"的合并收益，非纯通算掩盖——
   纯掩盖收益受 (T计算+T通信)/max(T计算,T通信) ≤ 2x 结构上限约束）
3. `destroy`/`uninit`/`MPI_Finalize` 干净退出

> 注 1：双 rank 会合已由早期 `/tmp` 文件轮询改为 `MPI_Barrier`（无残留文件，同步粒度
> 从 1ms 降到微秒级）。
> 注 2（**v3 kernel 时序修复，2026-09-17**）：v2 及之前 kernel 尾部的 head 前进诊断为
> "先睡后查 100ms"，每次 launch 固定 +100ms——旧基准 8.02x/7.87x 实为该固定睡眠的
> launch 次数摊薄（流水 1 次 vs 串行 8 次），并非真实通算掩盖。v3（时名 libmf_overlap_v3）
> 改为先查后睡 + 100µs 粒度后实机复测：**①流水 8×128KB=6.59ms、②串行×8=16.94ms、
> 加速比 2.57x**（占用态设备，功能与正确性口径不变，mismatch=0）。历史数字仅作
> 过程记录，勿作性能口径引用。

## 运行（C++，实机验证）

**一键（推荐）**：`bash run.sh`——本目录脚本自动完成环境自检（CANN 兜底探测/bisheng 检查/
MPI 两套自动识别）、主构建检查（静态库缺失自动补构建）、示例编译与双 rank 拉起；
亦支持 `--build-only` / `--run-only`。以下手工展开命令供理解与定制：

```bash
# 1. 仓库根一键构建(主库 + 编排 kernel 三件套 + A3 血统 kernel, NPU 构建自动全含;
#    需环境变量 ASCEND_HOME_PATH, 无 CANN 工具链的环境自动跳过 kernel 不阻断)
cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
cd examples/memory_pool/01_basic/08_310p_aicore_aicpu_sdma
#   MPI 取发行版一套(头/库/启动器须同源; 换 /usr/local/openmpi 时改用其路径与启动器)
MPI_INC=/usr/lib/aarch64-linux-gnu/openmpi/include
MPI_LIB=/usr/lib/aarch64-linux-gnu/openmpi/lib
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w -x asc 08_310p_aicore_aicpu_sdma.asc -x none \
  -o /tmp/ex08 -DOMPI_SKIP_MPICXX -I$MPI_INC <CANN include 路径> <仓库 include 路径> \
  -Wl,--start-group <build 下四个静态库: libmf_smem/libmf_hybm_core/libacc_tcp_net/libmf_acc_offload> \
  -Wl,--end-group -L$MPI_LIB -lmpi <CANN lib 路径> -lascendcl -lruntime -ldl -pthread -lm

# 2. mpirun 双 rank 运行(rank 由 MPI 分配, 不再传参; **设备侧零部署**——初始化自动
#    探测→CUST 迁移→CP1 搜索路径就位→完整性校验, 全新设备/环境无任何手工部署步骤;
#    CP1 目录树为 CANN 基线自带, 洁净卡实测可直接写入)
export MF_SDMA_ORCH_JSON=<build/src/hybm/ops/hybm_kernel/libmf_sdma_orch_v7.json>
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
export LD_LIBRARY_PATH=$ASCEND_HOME_PATH/aarch64-linux/lib64:$MPI_LIB:...
mpirun -np 2 /tmp/ex08   # root 用户加 --allow-run-as-root
# 期望输出: 双方 "=== EX08 rN PASS ===" + r0 掩盖量化加速比(首轮含自动部署秒级开销)

# 长驻 soak(可选, 验收监督续命): 跨 epoch 壁钟自退边界的持续收发零丢失
EX08_SOAK_ROUNDS=30000 bash run.sh --run-only   # ~60s, 期望 ③soak PASS + 跨 ≥3 次续命
```

## 与相关示例/实验的关系

- **07**：host 编排（BM API + `HostDataOpAclMemcpy`），数据面全在 host 线程
- **08**（本示例）：设备发起协同（SHM API + AICore×AICPU），通信与计算可重叠
- 机制配方与平台事实档案：`docs/310p/机制档案.md`（SQE 配方/提交链/状态机/错误码）、
  `docs/310p/平台事实.md`（协同模式实测事实）
