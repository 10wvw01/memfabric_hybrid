# 08_310p_die_pair_overlap：310P Duo 单卡双 die 2-rank AICore×AICPU 协同

## 场景

Atlas 300I Duo（Ascend310P3）单卡双 die（dev0/dev1）2-rank SHM 对称内存池上，
演示**设备发起的计算-通信协同**（与 07 的 host 编排互补）：

- **AICore kernel 产数据**（写池 GVA + `ENTIRE_DATA_CACHE` clean）
- **GM 邮箱委托**（16B 描述符 `{dataAddr, words}`，vector 直写 + 行 clean）
- **AICPU 编排 kernel 代投 SDMA SQE 对**（数据 + 8B 标志一次 doorbell，FIFO 保序
  保证"标志到即数据到"）——设备头原语 `smem_shm_aicore_overlap.h` + host API
  `smem_shm_overlap_launch*`
- **对端 AICore 消费**（dcci 轮询池内标志 → 读改写 INC → 回程）

全内存通知链（邮箱/池内标志/SQE 对），310P3 上 notify/事件对不可用（实测证伪）。

## 目标

- 验证 SHM API 设备发起语义在 310P 落地：`smem_shm_create(SDMA)` → 传输准备
  （CP 流 + 工作区 + 编排 kernel + 冒烟）全自动完成
- 验证 AICore 直接 GM 访问 AclVmm 池 GVA（产/读改写/dcci 轮询标志）
- **三阶段性能语义**：
  - ① mode13 八步闭环（单 chunk 1MB）——功能正确性（逐字校验）
  - ② mode13 串行 ×8（同步逐次）——无重叠对照
  - ③ mode14 多 chunk 流水（8×128KB）——**通信被计算掩盖**（加速比量化）

## 使用能力

`smem_shm_create`（SDMA 位 + `MF_310P_SHM_OVERLAP=1`）→ `smem_shm_overlap_get_workspace`
→ AICore kernel（`smem_shm_overlap_notify`/`poll_flag`/`get_flag_addr` 设备头原语）→
`smem_shm_overlap_launch` / `smem_shm_overlap_launch_pipelined` → `smem_shm_overlap_wait`
→ `smem_shm_overlap_get_result` → `smem_shm_destroy`

## 规模建议

- world_size=2 固定（同卡 die 对映射 dev0/dev1）
- 单 chunk ≤ 1MB（AICPU SQE 上限）；chunk 数 ≤ 64（邮箱槽数）
- 每 rank 段末尾 2MB 为协同标志保留区（勿存放用户数据）

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1
- 协同编排 kernel 已部署到设备 CP1（部署链：CUSTAICPUKernel mode=1 迁移落盘 + 设备内复制到
  `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`，机制与运维纪律见
  `310p-aicore-aicpu-开发进展.md` 附录 A.4；**每次正式运行前须过验证门**——
  deployer 保活到期退出会触发清理链删除设备侧文件）
- 环境变量：`MF_310P_SHM_OVERLAP=1` + `MF_310P_OVERLAP_JSON=<launch json 路径>`
  （json 由主构建 `mf_overlap_kernel` 目标产出）

## 验收标准

1. ①mode13：r0 终态数据 mismatch=0（1024 字逐字），双侧 kernel mainRet=0、AICore
   状态走满（0xA3/0xB3）、headAfter 前进（TSFW 消费证据）
2. ③mode14：8 chunk 全量（1MB）逐字校验 mismatch=0；最后一块结果标志到达
3. **掩盖量化**：③流水总时延 < ②串行总时延（输出加速比；理想情况流水稳态
   ≈ AICore 单 chunk 计算时间，通信开销不再线性叠加）
4. `destroy`/`uninit` 干净退出

## 运行（C++，实机验证）

```bash
# 1. 仓库根构建 + 示例编译(bisheng host+AICore 统一编译)
cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
cd examples/memory_pool/01_basic/08_310p_aicore_aicpu_overlap
bisheng --npu-arch=dav-2002 -O2 -std=c++17 -w -x asc 08_310p_aicore_aicpu_overlap.asc -x none \
  -o /tmp/ex08 <CANN include 路径> <仓库 include 路径> \
  -Wl,--start-group <build 下三个静态库> -Wl,--end-group <CANN lib 路径> -lascendcl -lruntime -ldl -pthread -lm

# 2. 部署 kernel 到设备(部署链见开发进展文档附录 A.4)
# 3. 双 rank 运行
export MF_310P_SHM_OVERLAP=1 MF_310P_OVERLAP_JSON=<libmf_overlap_v1.json 路径>
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0
export LD_LIBRARY_PATH=$ASCEND_HOME_PATH/aarch64-linux/lib64:...
/tmp/ex08 0 & /tmp/ex08 1 & wait
# 期望输出: 双方 "=== EX08 rN PASS ===" + r0 掩盖量化加速比
```

## 与相关示例/实验的关系

- **07**：host 编排（BM API + `HostDataOpAclMemcpy`），数据面全在 host 线程
- **08**（本示例）：设备发起协同（SHM API + AICore×AICPU），通信与计算可重叠
- 机制验收与配方档案：仓库根目录 `310p-aicore-aicpu-开发进展.md`
  （〇章实现总结 + E0a/E0b/E0c/P1 四级验收记录 + 附录 A 配方速查）
