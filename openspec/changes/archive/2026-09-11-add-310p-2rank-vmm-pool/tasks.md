> 补录说明：本 change 为已合入工作的补录，全部任务已完成。commit 对应关系：efea25d1（P1）、
> e4f22866（P2，含 F17 幂等去重与 F20 UAF 修复的合入态）、8a538d60（P3）、6ba6b33e（F18 分类修复）、
> 77a8fde1/5ec0e75a（P5 示例）、b0d3c720/a4db8996（评审修复）、5da86499/3b672268/78ab5de5（文档）。

## 1. SOC 识别与 ACL VMM 符号层（P1）

- [x] 1.1 `hybm_define.h` 新增 `ASCEND_310P` 枚举，`GetAscendSocType` 增 "Ascend310P" 前缀匹配（commit efea25d1；UT `DlAclApiVmmTest` GetAscendSocType 匹配用例 PASS）
- [x] 1.2 `dl_acl_api.h/.cpp` 新增 14 个 aclrt* VMM 符号的本地类型/typedef/`DL_LOAD_SYM_OPTIONAL`/inline wrapper/Cleanup 置空（commit efea25d1；UT `DlAclApiVmmTest(+Init)` 4/4 PASS，含符号 nullptr 判定）

## 2. HybmAclVmmSegment 内存段（P2）

- [x] 2.1 新增 `hybm_acl_vmm_segment.h/.cpp`：ReserveMemorySpace 整窗预留、AllocLocalMemory（MallocPhysical P2P_HUGE/NONE → MapMem → SetAccess）、Export（magic EE05 + 192B static_assert）、Import（die 对五步/跨卡直连）、Mmap、RemoveImported、CheckSdmaReaches（commit e4f22866；UT `HybmAclVmmSegmentTest` 6/6 PASS，MOCKER 14 符号状态机）
- [x] 2.2 `hybm_define.h` 增 `ACL_VMM_HBM_SLICE_EXPORT_INFO_MAGIC` 并扩展 `IsHbmSlice`（commit e4f22866；UT `ExInfoTransferTest` 16/16 PASS，含 EE05 扩展）
- [x] 2.3 `hybm_mem_segment.cpp` HBM 分支前插 310P SOC 分派（优先于 GVA 版本判断）+ `QueryDeviceInfoSocTolerant` 容忍降级（commit efea25d1；UT `HybmMemSegmentTest` 37/37 PASS，含 Create 分派与容忍降级用例）
- [x] 2.4 Import 幂等去重（`importKeys_` 按 rankId+sliceIndex），join 重试不因重复 import 报 107017（commit e4f22866；实机 e2e 复验通过，strace+日志定位确认）
- [x] 2.5 `ReleaseSliceMemory` use-after-free 修复（入口 `MemSlicePtr holder = slice` 拷贝提升引用计数）（commit e4f22866 合入态；正式 UT 二进制 ASAN 检出→修复后 `HybmAclVmmSegmentTest` 6/6 PASS）
- [x] 2.6 `ClassifyAddressMask` 扩展：allocatedMap 命中且非 imported → LOCAL|GLOBAL 双位（commit 6ba6b33e；UT `HybmVaManagerTest` 105/105 PASS，实机 G2H 方向校验恢复）

## 3. HostDataOpAclMemcpy 拷贝算子（P3）

- [x] 3.1 新增 `hybm_data_op_acl_memcpy.h/.cpp`：Initialize 惰性建内部 stream、DataCopy 方向分派（D2D async+SyncStream / H2D/D2H 同步 / 不支持方向 BM_INVALID_PARAM+ERROR 含 rankId/direction）、BatchDataCopy、DataCopyAsync 降级、Wait no-op、TransformVA 恒等（commit 8a538d60；UT `HybmDataOpAclMemcpyTest` 5/5 PASS）
- [x] 3.2 `hybm_compose_data_op.cpp` SDMA 位分支按 310P 实例化算子占 `sdmaDataOperator_` 槽位，`hybm_data_op_factory` 注册（commit 8a538d60；UT `HybmComposeDataOpTest` 21/21 PASS，含 310P 分支）

## 4. init 链实机确认（P4）

- [x] 4.1 实机编译后 `hybm_init(0,0)` + `hybm_uninit` 通过，零代码修改（F2/F3 预判成立；实机最小 main 验证）

## 5. Example 与实机验收（P5）

- [x] 5.1 新增 `examples/memory_pool/01_basic/07_310p_die_pair_pool/`（.cpp 双进程双池三阶段逐字校验 + .py spawn 版 + README 六段），不挂 run_all_examples.sh（commit 77a8fde1/5ec0e75a；实机运行双方各 18 步 OK、三 Phase mismatch=0、FAIL 数=0）
- [x] 5.2 实机验收 3/3 轮稳定复跑全绿，`npu-smi info` 确认无残留进程/占用（开发计划第一节状态记录）

## 6. 回归与文档（P6）

- [x] 6.1 全量 UT 回归：改动覆盖路径 177 用例 PASS（6 套件）；全量 90 套件逐套件模式 83 PASS/1700 用例，非通过项经纯基线对照证实为存量环境问题（开发计划第二节）
- [x] 6.2 pre-commit / clang-format v18.1.8（Allman、4 空格、120 列）通过（commit b0d3c720 补齐 license 头）
- [x] 6.3 PR 评审意见处置：20 条中 15 条修复、5 条延后登记 V2 TODO（commit a4db8996 + 开发计划第九节）
- [x] 6.4 开发计划文档固化（含 F1-F20 实测结论、D1-D10 决策、验证命令、附录 A1-A7 踩坑记录）（commit 5da86499/3b672268/78ab5de5；文档在仓库根 `310p-2rank-vmm-开发计划.md`）
