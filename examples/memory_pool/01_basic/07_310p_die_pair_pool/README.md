# 07_310p_die_pair_pool：310P Duo 单卡双 die 2-rank HBM+DRAM 双池

## 场景

Atlas 300I Duo（Ascend310P3）单卡双 die（dev0/dev1）上组建 2-rank BM 内存池：
- **HBM 池**：每 rank 贡献 32MB LPDDR 设备内存，走 310P 专用 **ACL VMM** 链路
  （`aclrtMallocPhysical` + shareable handle 导出 + die 对显式 SetContext 导入 + `aclrtMemcpyAsync` D2D）
- **DRAM 池**：每 rank 贡献 32MB host 内存，走 **HOST_SHM**（同节点共享内存，纯 POSIX 链路）

双池为两个独立 entity（HOST_SHM 要求纯 DRAM 池且禁与其他 opType 混用）。

## 目标

- 验证 310P 识别（`ASCEND_310P`）→ `HybmAclVmmSegment`（HBM 段）→ `HostDataOpAclMemcpy`（拷贝算子）全链
- 验证对称 GVA：两进程 `aclrtReserveMemAddress` 窗口基址一致（实测 0x12c140000000），
  `peer_rank_ptr` 纯算术寻址
- 验证跨 die 数据路径：H2G（host→对端 HBM）、G2H（对端→host）、G2G（D2D async 跨 die）
- 验证 DRAM 池 HOST_SHM 复用：H2GH/GH2H 跨 rank 校验

## 使用能力

`bm.initialize`（store 组网）→ `bm.create2`（双池）→ `peer_rank_ptr`（GVA 寻址）→
`copy_data`（H2G/G2H/G2G/H2GH/GH2H）→ `destroy`

## 规模建议

- world_size=2 固定（dev0/dev1 die 对映射）；更大规模参考方案文档的 4-rank/8-rank 设计
- 单池规格受 310P 约束：HBM ≤ 64GB/rank（本例 32MB 仅演示）、DRAM 建议 ≥ 2MB 对齐

## 必要条件

- Atlas 300I Duo（Ascend310P3）+ CANN ≥ 9.1（驱动 Innerversion V100R001C10+）
- 310P HBM 池依赖的关键驱动行为（实测裁决，详见方案文档）：
  `MallocPhysical` 需 `ACL_MEM_HANDLE_TYPE_NONE` + `ACL_MEM_P2P_HUGE`；
  export 需 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION`（默认启用 PID 白名单会拒绝对端 import）
- Python 版需 memfabric_hybrid wheel（pybind11 构建环境）；**C++ 版可直接源码编译运行**（见下）

## 验收标准

1. 双 rank 双池 join 成功，GVA 对称（own/peer 地址两进程视角互补一致）
2. HBM 池三阶段 8MB 逐字校验 mismatch=0：
   - PhaseA：r0 H2G 写对端 → r1 G2H 读回
   - PhaseB：G2G 跨 die（peer 前段 → own 后段）
   - PhaseC（DRAM 池）：H2GH/GH2H
3. `destroy`/`uninit` 干净退出（无 D 态挂死、无 double free）

## 运行（Python，实机已验证）

```bash
# 需先构建并安装 memfabric_hybrid wheel(含 310P 支持), 且需 torch(host 侧缓冲)
pip3 install <wheel 路径>/memfabric_hybrid-*.whl torch
python3 07_310p_die_pair_pool.py
# 期望输出: "07_310p_die_pair_pool ok"
```

## 运行（C++，实机已验证）```bash
# 1. 仓库根构建: cmake -B build -DXPU_TYPE=NPU -DBUILD_PYTHON=OFF && cmake --build build -j
# 2. 链接静态库编译本示例(INCLUDE 路径按仓库实际结构调整)
CANN=/usr/local/Ascend/cann-9.1.0
g++ -std=c++17 -w -o /tmp/p5_e2e 07_310p_die_pair_pool.cpp \
  -Wl,--start-group \
  build/src/smem/csrc/libmf_smem.a \
  build/src/hybm/csrc/libmf_hybm_core.a \
  build/src/acc_links/csrc/libacc_tcp_net.a \
  -Wl,--end-group \
  -Isrc/hybm/include -Isrc/smem/include/host -Isrc/hybm/csrc/transport \
  -Isrc/hybm/csrc -Isrc/hybm/csrc/common -Isrc/hybm/csrc/under_api -Isrc/hybm/csrc/entity \
  -Isrc/hybm/csrc/mm -Isrc/hybm/csrc/driver -Isrc/hybm/csrc/driver/userspace \
  -Isrc/hybm/csrc/driver/version -Isrc/hybm/csrc/ts_engine -Isrc/util/csrc \
  -pthread -ldl
export ASCEND_HOME_PATH=$CANN
export LD_LIBRARY_PATH=$CANN/aarch64-linux/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64/common
/tmp/p5_e2e 0 & /tmp/p5_e2e 1 & wait
# 期望输出: 双方 "=== P5 rN DONE ==="
```

## Q&A

- **Q：为何 HBM 池 data_op_type 仍传 SDMA？**
  A：SDMA 位是"设备侧拷贝"的语义槽位。310P 上工厂按 SOC 分派为 `HostDataOpAclMemcpy`
  （aclrtMemcpyAsync D2D），不创建 910B 的 STARS SQE 路径（该路径依赖的
  `HalHostRegister` 在 310P 上不可用）。
- **Q：join 后为什么还要同步一次才开始拷贝？**
  A：join 为动态成员语义——先 join 的 rank 在组内只有一个成员时立即返回，
  peer 的内存在其 join 完成后才完成 import/映射（对称池地址按算术即时可见，但映射就绪需对端 join）。
