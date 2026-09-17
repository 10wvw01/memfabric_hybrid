# 08_310p_die_pair_overlap：310P Duo 单卡双 die 2-rank AICore×AICPU 协同

## 场景

Atlas 300I Duo（Ascend310P3）单卡双 die（dev0/dev1）2-rank SHM 对称内存池上，
演示**设备发起的计算-通信协同**（与 07 的 host 编排互补）：

- **AICore kernel 产数据**（写池 GVA + `ENTIRE_DATA_CACHE` clean）
- **GM 邮箱委托**（16B 描述符 `{dataAddr, words}`，vector 直写 + 行 clean）
- **AICPU 编排 kernel 代投 SDMA SQE 对**（数据 + 8B 标志，每 chunk 一次 doorbell，
  FIFO 保序保证"标志到即数据到"）——设备头原语 `smem_shm_aicore_overlap.h` + host API
  `smem_shm_overlap_launch`
- **对端 AICore 消费**（dcci 轮询池内标志 → 读改写 INC → 回程）

全内存通知链（邮箱/池内标志/SQE 对），310P3 上 notify/事件对不可用（实测证伪）。

## 目标

- 验证 SHM API 设备发起语义在 310P 落地：`smem_shm_create(SDMA)` → 传输准备
  （CP 流 + 工作区 + 编排 kernel）全自动完成
- 验证 AICore 直接 GM 访问 AclVmm 池 GVA（产/读改写/dcci 轮询标志）
- **两阶段验收语义**：
  - ① 多 chunk 流水（8×128KB）——功能正确性（1MB 逐字校验）
  - ② 串行 ×8（chunks=1 同步逐次）——无重叠对照
  - 输出**通信被计算掩盖**的加速比量化（流水稳态 ≈ AICore 单 chunk 计算时间）

## 使用能力

`smem_shm_create`（SDMA 位 + `MF_310P_SHM_OVERLAP=1`）→ `smem_shm_overlap_get_workspace`
→ AICore kernel（`smem_shm_overlap_notify`/`poll_flag`/`get_flag_addr` 设备头原语）→
`smem_shm_overlap_launch`（chunks 1~64，chunks=1 即单块）→ `smem_shm_overlap_wait`
→ `smem_shm_overlap_get_result` → `smem_shm_destroy`

## 规模建议

- world_size=2 固定（同卡 die 对映射 dev0/dev1）
- 单 chunk ≤ 1MB（AICPU SQE 上限）；chunk 数 ≤ 64（邮箱槽数）
- 每 rank 段末尾 2MB 为协同标志保留区（勿存放用户数据）

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1
- MPI（双 rank 拉起与栅栏；本机两套可用，**编译与启动必须同一套**，混用会段错误：
  发行版 4.1.2：`/usr/bin/mpirun` + 头/库 `/usr/lib/aarch64-linux-gnu/openmpi/{include,lib}`
  （编译需 `-DOMPI_SKIP_MPICXX`）；自装 4.1.5：`/usr/local/openmpi` 全套）
- 协同编排 kernel 已部署到设备 CP1（部署链：CUSTAICPUKernel mode=1 迁移落盘 + 设备内复制到
  `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`，机制与运维纪律见
  `310p-aicore-aicpu-开发进展.md` 附录 A.4；**每次正式运行前须过验证门**——
  deployer 保活到期退出会触发清理链删除设备侧文件）
- 环境变量：`MF_310P_SHM_OVERLAP=1` + `MF_310P_OVERLAP_JSON=<launch json 路径>`
  （json 由主构建 `mf_overlap_kernel` 目标产出）

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
> launch 次数摊薄（流水 1 次 vs 串行 8 次），并非真实通算掩盖。v3（libmf_overlap_v3）
> 改为先查后睡 + 100µs 粒度后实机复测：**①流水 8×128KB=6.59ms、②串行×8=16.94ms、
> 加速比 2.57x**（占用态设备，功能与正确性口径不变，mismatch=0）。历史数字仅作
> 过程记录，勿作性能口径引用。

## 运行（C++，实机验证）

```bash
# 1. 仓库根构建 + 示例编译(bisheng host+AICore 统一编译, 含 MPI)
cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
cd examples/memory_pool/01_basic/08_310p_aicore_aicpu_overlap
#   MPI 取发行版一套(头/库/启动器须同源; 换 /usr/local/openmpi 时改用其路径与启动器)
MPI_INC=/usr/lib/aarch64-linux-gnu/openmpi/include
MPI_LIB=/usr/lib/aarch64-linux-gnu/openmpi/lib
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w -x asc 08_310p_aicore_aicpu_overlap.asc -x none \
  -o /tmp/ex08 -DOMPI_SKIP_MPICXX -I$MPI_INC <CANN include 路径> <仓库 include 路径> \
  -Wl,--start-group <build 下三个静态库> -Wl,--end-group \
  -L$MPI_LIB -lmpi <CANN lib 路径> -lascendcl -lruntime -ldl -pthread -lm

# 2. 部署 kernel 到设备(部署链见开发进展文档附录 A.4)
# 3. mpirun 双 rank 运行(rank 由 MPI 分配, 不再传参)
export MF_310P_SHM_OVERLAP=1 MF_310P_OVERLAP_JSON=<libmf_overlap_v3.json 路径>
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
export LD_LIBRARY_PATH=$ASCEND_HOME_PATH/aarch64-linux/lib64:$MPI_LIB:...
mpirun -np 2 /tmp/ex08   # root 用户加 --allow-run-as-root
# 期望输出: 双方 "=== EX08 rN PASS ===" + r0 掩盖量化加速比
```

## 与相关示例/实验的关系

- **07**：host 编排（BM API + `HostDataOpAclMemcpy`），数据面全在 host 线程
- **08**（本示例）：设备发起协同（SHM API + AICore×AICPU），通信与计算可重叠
- 机制验收与配方档案：仓库根目录 `310p-aicore-aicpu-开发进展.md`
  （〇章实现总结 + E0a/E0b/E0c/P1 四级验收记录 + 附录 A 配方速查）
