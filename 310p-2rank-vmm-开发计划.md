# 310P 单卡双 die 2-rank VMM 内存池开发计划（V1，纯 aclrt* 路径）

> **状态：已完成并实机验收通过（3/3 轮稳定复跑全绿）**
> - P0-P6 全部完成；实机裁决补充见下表 F16-F20
> - 实机验收凭证：`examples/memory_pool/01_basic/07_310p_die_pair_pool/07_310p_die_pair_pool.cpp`
>   （双 rank 双池三阶段 8MB 逐字校验 mismatch=0，干净销毁）
> - 全量 UT 已完成：83/90 套件 PASS（1700/1700 用例），非通过项经纯基线对照证实为存量问题（见第二节）
> - **UT / example 复现命令见第八节（可直接复制执行）**；**V2 候选改进（PID 白名单恢复、公开 API 设备查询、算子真异步）见第九节 TODO**
>
> 目标：在当前 Atlas 300I Duo（310P3）机器上，**单卡双 die（dev0/dev1）2-rank** 建池实测通过——
> HBM 池（ACL VMM shareable handle + SetContext 导入 + `aclrtMemcpyAsync` D2D）与
> DRAM 池（HOST_SHM 复用）双池端到端数据校验。
> 上游设计文档：`memfabric-310p-内存池化方案.md`（D1-D9 决策、V1-V8 验证项）。

## 一、实机探测结论（已完成，全部实测验证）

| # | 事实 | 证据 | 对计划的影响 |
|---|------|------|-------------|
| F1 | 8 设备 = 4 卡×2 die | `npu-smi info -m`：Chip Logic ID 0-7，"Ascend 310P3"；dev0/dev1 同 NPU ID 5664 | dev0/dev1 同卡 die 对 ✓ |
| F2 | 驱动 `Innerversion=V100R001C10SPC008B221` | `/usr/local/Ascend/driver/version.info` | 落 V5 窗口（C10~C14）→ 现有代码按 **GVA V4** 处理，`HalGvaPrecheck` 原样通过 |
| F3 | driver HAL 24 个硬性符号全部存在 | `nm -D /usr/local/Ascend/driver/lib64/driver/libascend_hal.so` | **hybm_init 链零修改** |
| F4 | libascendcl.so 导出全部所需 aclrt* VMM 符号 | `nm -D` 实测 | D1 可行 |
| F5 | rt 符号经 libruntime.so 依赖链可解析 | `ldd libascendcl.so` | InitDeviceInfo 通过 |
| F6 | stream 封装已存在 | `dl_acl_api.h:221-301` | 算子直接复用 |
| F7 | 精确签名已核对 | `acl_rt.h`（Export:3273/Import:3301/SetAccess:5446/PhysicalMemProp:367 等） | typedef 照抄 |
| F9 | **soc 字符串 = "Ascend310P3"**（V8 ✓） | probe_single 实测 | 匹配子串 "Ascend310P" |
| F10 | **MallocPhysical：handleType 必须 NONE**（POSIX 全失败 100000）；memAttr P2P_HUGE OK（V5 ✓）；granularity 2MB | probe_memattr 矩阵实测 | prop.handleType = ACL_MEM_HANDLE_TYPE_NONE |
| F11 | **双进程 ReserveMemAddress base 一致 = 0x12c140000000**（V7 ✓，128MB 窗口） | probe_two_proc 实测 | 对称 GVA 成立，无需 expectPtr |
| F12 | **V3 裁决：export 必须带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION(0x1)`** —— 默认 flags=0 启用 PID 白名单，对端 import 被内核拒 -EACCES→507899（strace 实证 ioctl 返回） | probe_export_matrix + strace 实测 | Export 固定 flags=0x1，免 pid 交换（D7 简化为无白名单）；**V2 改进见 TODO-1（aclrtDeviceGetBareTgid + SetPidToShareableHandle 恢复白名单）** |
| F13 | **die 对 import 五步（D6 ✓）**：CreateContext(对端die)→SetCurrentContext→Import(sh,本端die)→SetDevice(本端die)→DestroyContext 全链成功 | probe_two_proc 双侧 OK | 按设计落地 |
| F14 | **VMM VA 是设备地址**：CPU 不可直接解引用（段错误），host 访问必须 `aclrtMemcpy` | probe_single 实测 | 算子 H2D/D2H 全走 aclrtMemcpy（与 D8 一致） |
| F15 | **跨进程 H2D 写对端 HBM ✓ + D2D async 跨 die 拷贝 ✓ + 双侧有序拆除 ✓**（16MB 逐字校验 mismatch=0） | probe_two_proc ALL PASS | 主数据路径可行性实证 |
| F16 | **310P 不支持 SDID/SERVER_ID/SUPER_POD_ID 查询**（rtGetDeviceInfo 返回 207000）→ InitDeviceInfo 加 `QueryDeviceInfoSocTolerant`（310P 降级为无效值+WARN，可达性改由段映射状态判定） | e2e 实测（hybm_mem_segment.cpp:165） | 建池前置适配；**V2 改进见 TODO-2（改用公开 API aclrtGetDeviceInfo）** |
| F17 | **join 重试会重复 import 同一 shareableHandle** → 107017 → `Import` 幂等去重（按 rankId+sliceIndex） | e2e 实测（strace+日志定位） | 段健壮性修复 |
| F18 | **本地池地址分类依赖 reservedMap**（AclVmm 段不登记）→ G2H 方向校验失败 → `ClassifyAddressMask` 扩展：allocatedMap 命中且非 imported → LOCAL\|GLOBAL 双位（与 reservedMap 分支语义一致，对 910B 行为等价） | e2e 实测 | 公共路径修改（需全量 UT 回归） |
| F19 | **join 为动态成员语义**：先 join 的 rank 单成员即返回 OK，peer 映射在对端 join 完成后才就绪 → 使用方需在全员 join 后同步再拷贝（现有 Python 示例的 Barrier 模式即正确姿势） | e2e 实测 | example/使用方契约 |
| F20 | **全量 UT 发现 `ReleaseSliceMemory` use-after-free**（ASAN 独有检出：入参为 map 内 shared_ptr 的引用，`slices_.erase` 后再访问 `slice->index_`；析构路径下 map 为最后所有者时触发）→ 入口 `MemSlicePtr holder = slice` 拷贝修复 | 正式 UT 二进制（ASAN+gcov）实测 | 段健壮性修复（手工链接非 ASAN 版漏检） |

## 二、全量 UT 回归结论（本机，逐套件独立进程模式）

> 本机首次具备 UT 运行条件（子模块已拉取、pybind11 已装）。`run_ut.sh` 单进程串行模式会因
> mockcpp API-hook 与 ASAN fake-stack 的存量不兼容在多个套件中途崩溃（见下），故采用
> **逐套件独立进程**模式取得确定性结果。

**结果：83/90 套件 PASS（1700/1700 用例全过），非通过项全部经纯基线对照证实为存量环境问题，与本方案改动无关：**

| 套件 | 现象 | 纯基线对照 | 根因 |
|---|---|---|---|
| HybmDataOpSdmaTest | ASAN 崩溃（44/52 后） | **相同进度崩溃** | 存量：mockcpp AArch64 机器码打桩（覆写函数前 16 字节）与 ASAN fake-stack 不兼容，进程内状态累积触发，布局敏感 |
| SmemBmTest | ASAN 崩溃（109/127 后） | **相同进度崩溃** | 同上 |
| SmemHaConfigStoreTest | ASAN 崩溃（14/33 后） | **相同进度崩溃** | 同上 |
| SmemShmTest | ASAN 崩溃（17/27 后） | **相同进度崩溃** | 同上 |
| NetworkEndpointUtilTest | 3 例失败（ipv6） | 相同失败 | 本机无 IPv6 地址 |
| SmemNetTest / SmemTransTest | GetLocalIpWithTarget / ipv6+超时 | 相同失败 | 本机网络环境（无 IPv6） |

**本方案改动覆盖路径全部通过**：HybmVaManagerTest 105/105（含 ClassifyAddressMask 修改）、
HybmMemSegmentTest 37/37（含 Create 分派与 QueryDeviceInfoSocTolerant）、HybmComposeDataOpTest 21/21
（含 310P 算子分支）、HybmDataOpAclMemcpyTest 5/5、DlAclApiVmmTest(+Init) 4/4、
HybmAclVmmSegmentTest 6/6（UAF 修复后）、ExInfoTransferTest 16/16（含 EE05 扩展）。
实机 e2e 修复后复验通过。

## 三、V1 设计决策

| # | 决策 | 依据 |
|---|------|------|
| D1 | 全部走 `DlAclApi` aclrt* 符号（12 个新增，`DL_LOAD_SYM_OPTIONAL` 加载） | 用户指定；OPTIONAL 保证 910B/950 机器零影响（符号缺失仅 WARN，指针保持 nullptr） |
| D2 | 新增 `HybmAclVmmSegment`（不动 HybmVmmBasedSegment） | 910C/950 VMM 段走 HAL halMem*，310P 走 ACL 层，仅接口名相似 |
| D3 | 新算子 `HostDataOpAclMemcpy` **占 `sdmaDataOperator_` 槽位**（compose Initialize:32-41 加 310P 分支） | `HostComposeDataOp::Wait`（:229-241）硬绑 `sdmaDataOperator_->Wait()`，不占此槽 `hybm_wait` 恒 BM_ERROR |
| D4 | exchange 新 magic `0xAABB1234FFFFEE05` + **必须扩展 `IsHbmSlice`** | `ImportForSegment`（entity:472-501）对未知 magic 静默跳过 → 192B 块留在 reader → 后续 transportKey 读偏断链 |
| D5 | VA：`aclrtReserveMemAddress(rankCnt×maxSize, NULL)` 整窗，`slot_p = base + p×maxSize`；登记 `AddVaInfo{gva=dva=hva=VMM VA}`（对齐 DevLegacy:181 填法）；**不走 VaManager 窗口分配** | 实测 VMM VA 0x12c140000000 在现有窗口（SVM 8T/GVM 40T+/A5）之外；池内地址全经 map 登记则分类安全 |
| D6 | die 对 import 显式 SetContext 五步：CreateContext(对端die)→SetCurrentContext→ImportFromShareableHandle(sh, 本端die)→SetDevice(本端die)→DestroyContext | 方案文档 §10：本进程 ctx 直接 import 被 P2P 前置检查拒 507899；前置条件（本端 ctx 存在）由 InitDeviceInfo 的 AclrtSetDevice 保证 |
| D7 | **先不设 pid 白名单**；import 失败（权限类错误码）时再加 `aclrtMemSetPidToShareableHandle` | §4/§9 实测未要求白名单；避免 V1 引入 pid 交换时序耦合 |
| D8 | D2D 必须 `aclrtMemcpyAsync` + SynchronizeStream（同步 D2D 跨 die 507899）；H2D/D2H 用同步 `aclrtMemcpy` | §2/§13 实测 |
| D9 | HBM 池 opType=SDMA 位（用户侧语义不变）；DRAM 池 opType=HOST_SHM（纯 POSIX 复用，已确认 310P 可用） | HOST_SHM 池 hybm_wait 恒失败是既有行为，example 不调用 |
| D10 | DRAM 池与 HBM 池 = **两个独立池（两个 entity）** | HOST_SHM 要求纯 DRAM 池（smem_bm.cpp:218-223）且禁与其他 opType 混用（:224-231） |

## 四、分阶段任务

### P0 探测程序（/home/t00842504/memfabfric_tmp，不进仓库，~0.5 天）

| 程序 | 内容 | 裁决的验证项 |
|------|------|--------------|
| probe_single.c | 单进程 dev0：GetSocName 打印 / Granularity 查询 / MallocPhysical(P2P_HUGE)→ReserveMemAddress→MapMem→SetAccess→host 读写自校验→FreePhysical 全链 | V8（soc 字符串）、V5（memAttr）、基本链路可用性 |
| probe_two_proc.c | 双进程（父 fork 或独立启动 + argv 传 sh）：各自 `ReserveMemAddress(2×64MB, NULL)` 打印 base 比对一致性；随后 r0 export→管道传 sh→r1 SetContext 五步 import→MapMem→双进程交叉写读校验→有序拆除 | V7（小规模 base 一致性）、D6（ctx 五步）、D7（免白名单）、FreePhysical 多进程语义 |

### P1 SOC 识别 + 符号层（~300 行 + UT）

1. `src/hybm/csrc/common/hybm_define.h:99-104`：`AscendSocType` 尾部增 `ASCEND_310P`
2. `src/hybm/csrc/under_api/dl_acl_api.cpp:148-155`：GetAscendSocType 增 `"Ascend310P"` 匹配（以 P0 实测字符串为准）
3. `dl_acl_api.h/.cpp`：新增本地类型（`aclrtPhysicalMemProp`/`aclrtMemAccessDesc`/枚举）+ 12 符号的 typedef/静态指针/DL_LOAD_SYM_OPTIONAL/inline wrapper/Cleanup 置空，全部沿用现有模式（参照 `dl_acl_api.cpp:114` aclrtMemcpyBatch 的 OPTIONAL 先例）
4. UT：`test/ut/testcase/hybm/under_api/dl_acl_api_vmm_test.cpp`（加载+nullptr 判定）+ GetAscendSocType 匹配（改 stub aclrtGetSocName 返回值）

### P2 HybmAclVmmSegment（~600 行 + UT，核心）

1. 新文件 `src/hybm/csrc/mm/hybm_acl_vmm_segment.h/.cpp`，接口对齐 MemSegment 纯虚：
   - `ReserveMemorySpace`：`aclrtReserveMemAddress(totalSize, NULL)` 整窗 → 记录 base/totalSize → `*address = base`（GVA==DVA==HVA 同值，对齐 D5）
   - `AllocLocalMemory`：granularity 对齐 → `aclrtMallocPhysical(P2P_HUGE)` → `aclrtMapMem(lvaBase_+offset)` → `aclrtMemSetAccess(RW)` → MemSlice(MEM_PT_TYPE_GVM) + AddVaInfo
   - `Export(slice)`：`aclrtMemExportToShareableHandle` → `AclVmmExportInfo{magic=EE05, gva, slotIndex, devId, pid, rankId, size, shareableHandle}`（static_assert 192B）→ LiteralExInfoTranslater 序列化
   - `Import`：按对端 devId 判 die 对（同卡：`devId/2` 相同）→ D6 五步；跨卡本进程 ctx 直接 import；`addresses[i]=info.gva`
   - `Mmap`：对每个 import `aclrtMapMem(slot_p, handle)` + `SetAccess` → `AddVaInfoFromExternal`
   - `RemoveImported`：`aclrtUnmapMem` + `aclrtFreePhysical(import handle)`（暂不调 DisablePeerAccess——die 对无 P2P，2-rank 场景零槽位）
   - `CheckSdmaReaches(rankId)`：2-rank die 对恒 true（仅按本段 imports_ 判定）
2. `hybm_define.h:72` 后增 `ACL_VMM_HBM_SLICE_EXPORT_INFO_MAGIC = 0xAABB1234FFFFEE05`；`IsHbmSlice`（:86-89）增该值
3. `hybm_mem_segment.cpp:82-88`：HBM 分支前插 `socType_ == ASCEND_310P → HybmAclVmmSegment`（**soc 判断优先于 GVA 版本判断**——310P 驱动被映射为 V4 也不能落 halMem* 路径）
4. UT：`test/ut/testcase/hybm/mm/hybm_acl_vmm_segment_test.cpp`（照抄 vmm_based_segment_test 模式 + MOCKER 12 个新符号 wrapper）

### P3 HostDataOpAclMemcpy（~400 行 + UT）

1. 新文件 `src/hybm/csrc/data_operation/host/hybm_data_op_acl_memcpy.h/.cpp`：
   - `Initialize`：lazy 建内部 stream（`AclrtCreateStream`）——不碰 HalHostRegister/paramSpace
   - `DataCopy`：GD2GD/LD2GD/GD2LD → TransformVa 后 `AclrtMemcpyAsync(DEVICE_TO_DEVICE)` + SynchronizeStream；LH2GD/GD2LH → 同步 `AclrtMemcpy(H2D/D2H)`；不支持方向 BM_INVALID_PARAM + ERROR（含 rankId/direction）
   - `BatchDataCopy`：逐条循环；`DataCopyAsync`：内部转同步执行；`Wait`：BM_OK（no-op）；`TransformVa`：no-op（GVA==DVA==HVA，地址直接可用）
2. `hybm_compose_data_op.cpp:32-41`：SDMA 位分支内按 `GetAscendSocType()==ASCEND_310P` 实例化 HostDataOpAclMemcpy（占 sdmaDataOperator_ 槽）
3. UT：`test/ut/testcase/hybm/data_operator/hybm_data_op_acl_memcpy_test.cpp`（照抄 host_shm 算子测试模式，MOCKER AclrtMemcpy/MemcpyAsync/SynchronizeStream 状态机）

### P4 init 链实机确认（预计零代码）

实机编译后跑最小 main：`hybm_init(0,0)` + `hybm_uninit`。F2/F3 已证明通过；若意外失败再按实测错误码补 gate（不预先修改）。

### P5 example + 实机验收

1. `examples/memory_pool/01_basic/07_310p_die_pair_pool/`：
   - `07_310p_die_pair_pool.py`：multiprocessing(spawn) 双进程（rank0=dev0、rank1=dev1，硬编码）；rank0 兼 store（cfg.start_store）；**两个池**：HBM 池 `bm.create2(local_hbm_size=1GB, data_op_type=SDMA)` + DRAM 池 `bm.create2(local_dram_size=512MB, data_op_type=HOST_SHM)`；校验流：HBM 池 H2G→G2G→G2H 往返逐字校验 + DRAM 池 host 往返；成功打印 `07_310p_die_pair_pool ok`
   - `README.md`：六段（场景/目标/使用能力/规模建议/必要条件/验收标准）+ 运行段
   - 不挂 run_all_examples.sh（310P 专用示例，避免影响 910B CI）
2. 实机验收：`python3 07_310p_die_pair_pool.py` 通过；`npu-smi info` 确认无残留进程/占用

### P6 回归

1. `bash script/run_ut.sh --fast`（全量 UT 绿；新文件需 touch CMakeLists 触发 reconfigure——GLOB 无 CONFIGURE_DEPENDS）
2. pre-commit / clang-format v18.1.8（Allman、4 空格、120 列）
3. 实机复跑 example ×3（稳定性）

## 五、代码规范（每个 P 内置）

- 错误返回路径必须 ERROR 日志 + 关键参数（rankId/devId/slotIndex/size/ret/shareableHandle）
- 函数 ≤50 行（超限拆分）；嵌套 ≤4 层；行宽 ≤120
- 新增 .cpp 自动被 `hybmm_objects` GLOB 与 UT GLOB 收编（无需注册 CMake，但需触发 reconfigure）

## 六、风险与对策

| # | 风险 | 对策 |
|---|------|------|
| R1 | 双进程 reserve base 不一致（V7） | P0 验证；失败改 `expectPtr` 指定同址；再失败回退 IPC+查表（方案文档已备案） |
| R2 | die 对 import 失败（507899 之外的错误码） | 对照方案文档 §10 调用链逐层定位；D7 失败时加 pid 白名单 |
| R3 | `aclrtMapMem` 到指定 slot 失败 | P0 覆盖（MapMem 显式地址）；失败则槽位窗口方案重评 |
| R4 | 裸 VMM 地址被 GetLocalMemoryType 误分类（0x12c1… 在 SVM 窗外） | V1 池内地址全经 AddVaInfo 登记（ClassifyAddressMask 先查 map 后查范围）；RegisterMemory（用户注册）路径 V1 不承诺，登记为已知限制 |
| R5 | HBM 池 G2G 候选链未命中 | entity 默认 tag（"localTag:SDMA:localTag"）+ D9 SDMA 位 + P3 占槽，三重保证 |
| R6 | example 双池 store 端口冲突 | 沿用独立端口约定（8576） |
| R7 | DRAM 池 hybm_wait 失败 | 既有行为（HOST_SHM 无 sdma 算子），example 不调用 |

## 七、里程碑

```
M1 = P0 + P1        → 探测结论 + 符号层就绪（可独立合入，零行为变化）
M2 = M1 + P2        → 2-rank HBM 池 hybm 层闭环（探测程序级验证）
M3 = M2 + P3 + P4   → smem_bm 全链可用
M4 = M3 + P5 + P6   → 实机 example 验收（最终目标）
```

## 八、验证执行命令（UT / Example，仓库根目录直接运行）

> 前置条件：主构建已完成（`build/` 下三个静态库存在、`output/bin/ut/test_memfabric` 为最新）。
> 封装脚本等价物位于 `/home/t00842504/memfabfric_tmp/run_310p_ut.sh`、`run_310p_example.sh`。

### 8.1 UT：新增套件 + 改动覆盖路径回归（177 用例，~1 分钟）

```bash
cd /home/t00842504/memfabric_hybrid/output/bin/ut && \
export LD_LIBRARY_PATH=/home/t00842504/memfabric_hybrid/output/smem/lib64:/home/t00842504/memfabric_hybrid/output/hybm/lib64:/home/t00842504/memfabric_hybrid/output/hybm/lib64/cann/driver/lib64 && \
export ASCEND_HOME_PATH=/home/t00842504/memfabric_hybrid/output/hybm/lib64/cann && \
export ASAN_OPTIONS="detect_stack_use_after_return=1:allow_user_poisoning=1" && \
./test_memfabric --gtest_filter="*DlAclApiVmmTest.*:*HybmAclVmmSegmentTest.*:*HybmDataOpAclMemcpyTest.*:*HybmVaManagerTest.*:*HybmMemSegmentTest.*:*HybmComposeDataOpTest.*" 2>&1 | grep -E "\[==========\] Running|\[  PASSED  \]|\[  FAILED  \]|ERROR: Address"
```

预期输出：
```
[==========] Running 177 tests from 6 test suites.
[  PASSED  ] 177 tests.
```

### 8.2 UT：全量 90 套件逐套件扫描（规避存量 mockcpp+ASAN 崩溃，~10 分钟）

```bash
cd /home/t00842504/memfabric_hybrid/output/bin/ut && \
export LD_LIBRARY_PATH=/home/t00842504/memfabric_hybrid/output/smem/lib64:/home/t00842504/memfabric_hybrid/output/hybm/lib64:/home/t00842504/memfabric_hybrid/output/hybm/lib64/cann/driver/lib64 && \
export ASCEND_HOME_PATH=/home/t00842504/memfabric_hybrid/output/hybm/lib64/cann && \
export ASAN_OPTIONS="detect_stack_use_after_return=1:allow_user_poisoning=1" && \
./test_memfabric --gtest_list_tests 2>/dev/null | grep -vE "^  " | sed 's/.$//' | while read s; do \
  OUT=$(timeout 300 ./test_memfabric --gtest_filter="${s}.*" 2>&1); C=$?; \
  N=$(echo "$OUT" | grep -c '^\[       OK \]' || true); \
  if [ $C -eq 0 ]; then echo "PASS  $s ($N)"; \
  elif echo "$OUT" | grep -q AddressSanitizer; then echo "ASAN-CRASH(存量)  $s"; \
  else echo "FAIL(环境)  $s"; fi; \
done | tee /tmp/opencode/my_ut_all.log | sort | uniq -c | head -5; echo "===== 汇总 ====="; grep -c "^PASS" /tmp/opencode/my_ut_all.log; grep -vc "^PASS" /tmp/opencode/my_ut_all.log
```

预期：`PASS=83`、非 PASS=7（4 个 ASAN-CRASH 存量 + 3 个网络环境，均经纯基线对照证实与本次改动无关，详见第二节）。

### 8.3 Example：编译（一次性，~30 秒，产物 `/tmp/opencode/p5_e2e`）

```bash
cd /home/t00842504/memfabric_hybrid/build && \
g++ -std=c++17 -w -o /tmp/opencode/p5_e2e \
  ../examples/memory_pool/01_basic/07_310p_die_pair_pool/07_310p_die_pair_pool.cpp \
  -Wl,--start-group src/smem/csrc/libmf_smem.a src/hybm/csrc/libmf_hybm_core.a src/acc_links/csrc/libacc_tcp_net.a -Wl,--end-group \
  -I../src/hybm/include -I../src/smem/include/host -I../src/hybm/csrc/transport \
  -I../src/hybm/csrc -I../src/hybm/csrc/common -I../src/hybm/csrc/under_api -I../src/hybm/csrc/entity \
  -I../src/hybm/csrc/mm -I../src/hybm/csrc/driver -I../src/hybm/csrc/driver/userspace \
  -I../src/hybm/csrc/driver/version -I../src/hybm/csrc/ts_engine -I../src/util/csrc \
  -pthread -ldl && echo BUILD_OK
```

### 8.4 Example：运行（r0=dev0 + r1=dev1 双进程双池，~40 秒）

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0 && \
export LD_LIBRARY_PATH=/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64/common && \
rm -rf /tmp/p5_e2e && cd /tmp/opencode && \
(stdbuf -o0 ./p5_e2e 0 > ex_r0.log 2>&1 &) && (stdbuf -o0 ./p5_e2e 1 > ex_r1.log 2>&1 &) && \
sleep 40 && echo "===== r0 =====" && grep -E "\[r0 (OK|FAIL|INFO)|DONE|Phase" ex_r0.log && \
echo "===== r1 =====" && grep -E "\[r1 (OK|FAIL|INFO)|DONE|Phase" ex_r1.log && \
echo "===== 汇总: FAIL数=$(cat ex_r0.log ex_r1.log | grep -c FAIL || true) ====="
```

预期：双方各 18 步 OK、三个 Phase `mismatch=0`（PhaseA 跨进程 H2G/G2H、PhaseB G2G 跨 die、PhaseC DRAM H2GH/GH2H）、`=== P5 rN DONE ===`、`FAIL数=0`。日志明细在 `/tmp/opencode/ex_r0.log`、`ex_r1.log`。

## 九、后续 TODO（V2 候选改进）

### TODO-1：PID 白名单方案（替代 DISABLE_PID_VALIDATION，提升安全性）

**现状（V1）**：export 固定带 `ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION(0x1)` 关闭白名单校验
（F12 实测裁决：默认 flags=0 启用白名单，对端 import 被内核拒 -EACCES→507899）。这放弃了驱动级
PID 隔离语义，多租户场景存在越权访问风险。

**改进方案**：恢复白名单，用公开 API 获取 pid 并显式授权：
1. 每个 rank 用 `aclrtDeviceGetBareTgid`（公开 API，acl_rt.h:3367，本机 libascendcl.so 已导出）
   获取本进程 bare tgid，写入自己的 `AclVmmExportInfo.pid` 字段随 exchange_info 分发
   （该字段已存在，当前填的是 `MemSegment::pid_`，改用公开 API 获取即可）
2. exporter 在 join 阶段收到 peer 的 exchange_info 后，对**自己已导出的每个 shareableHandle**
   补调 `aclrtMemSetPidToShareableHandle(sh, &peerPid, 1)` 将 importer 加入白名单
   （`DlAclApi::AclrtMemSetPidToShareableHandle` 封装已就绪）
3. export 不再带 DISABLE flag，对端正常 import

**时序设计**：export 发生在建池（AllocLocalMemory→ExportInner），peer pid 在 join 才可得——
`SetPidToShareableHandle` 是对 shareableHandle 的独立授权操作，可在 export 之后补调；
挂接点建议放在 `HybmAclVmmSegment::Import`（解析到 peer 的 pid 时对本段 exportMap_ 全部 handle 补授权）。

**前置探测（P0 补充）**：V3 复测——export(默认 flag) → SetPidToShareableHandle(peerPid) →
peer import 的完整时序在 310P 是否放行（确认 export 后补调授权生效）。

### TODO-2：设备信息查询改用公开 API `aclrtGetDeviceInfo`（摆脱 rt 内部符号依赖）

**现状（V1）**：`MemSegment::InitDeviceInfo` 经内部符号 `rtGetDeviceInfo` 查询
SDID/SERVER_ID/SUPER_POD_ID，310P 上全部返回 207000（F16），已做容忍降级
（`QueryDeviceInfoSocTolerant`：310P 降级为无效值+WARN，可达性改由段映射状态判定）。

**改进方案**：
1. `dl_acl_api` 新增 `aclrtGetDeviceInfo`（公开 API，acl_rt.h:3890，签名
   `aclrtGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attr, int64_t *value)`，本机已确认导出）
   的封装（`DL_LOAD_SYM_OPTIONAL` 加载，沿用 14 符号集模式）
2. 310P 分支改走该接口：先实机探测 `aclrtDevAttr` 枚举各属性在 310P 的可用性矩阵
   （枚举含 `ACL_DEV_ATTR_NPU_ARCH=601`、`ACL_DEV_ATTR_IS_VIRTUAL=501` 等），
   若存在 SDID/serverId 等价属性则恢复拓扑坐标判定；若无则维持容忍降级（行为不变）
3. 顺带评估以 `aclrtDeviceGetBareTgid` 替代内部符号 `rtDeviceGetBareTgid`（InitDeviceInfo 现用）

**收益**：消除对 `rtGetDeviceInfo`/`rtDeviceGetBareTgid` 等 rt 内部符号的依赖（这些符号无版本
兼容承诺），CANN 升级兼容性更好。

### TODO-3：算子真异步支持（兑现 ASYNC_COPY_FLAG / 外部 stream / hybm_wait 契约）

**现状（V1 有意简化）**：`HostDataOpAclMemcpy` 全方向等效同步——H2D/D2H 仅同步 `aclrtMemcpy`；
D2D 为 `AclrtMemcpyAsync` 提交后**立即** `SynchronizeStream`；`DataCopyAsync` 直接降级调
`DataCopy`，`Wait` 为 no-op。用户传 `ASYNC_COPY_FLAG` 或外部 stream 时得到阻塞行为，
**异步契约未兑现**。

**与 910B 的差距**：`HostDataOpSDMA` 的 `DataCopyAsync` 支持真异步提交（内部 HybmStream 或
外部 aclrtStream），由 `smem_bm_wait` 统一等待，可实现拷贝间流水线重叠（多段 D2D、H2D 预取场景）。

**改进方案**：
1. H2D/D2H 增加异步路径：`AclrtMemcpyAsync(H2D/D2H, stream)` 提交即返回（外部 stream 优先，
   否则内部 stream_）
2. D2D 延迟同步：`CopyDeviceToDevice` 拆为"提交"与"等待"两步，`SynchronizeStream` 移到
   `Wait()`（`waitId` 槽位语义已有，compose 的 Wait 硬绑 sdmaDataOperator_ 槽位正好承接）
3. 同步语义（无 flag 无 stream）保持现状：提交后立即等——行为不变，零回归风险
4. 验收：e2e 增加异步路径用例（`smem_bm_copy(..., ASYNC_COPY_FLAG)` + `smem_bm_wait` 逐字校验）；
   带宽对比确认多段拷贝场景收益
