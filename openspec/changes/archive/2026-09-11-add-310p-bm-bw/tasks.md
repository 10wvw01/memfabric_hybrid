> 补录说明：本 change 为已合入工作的补录，全部任务已完成。commit 对应关系（PR !3，branch 310p-bm-bw，merge 0320410e）：
> e78a6faa（枚举对齐 + P2P_HUGE→HBM_HUGE 回退 + UT，含原 f4574999/01f993db）、
> 7b548da7（benchmark/bm_310p 全套，含原 e0305603/d30c…三 commit 重组：用例 + pinned/pageable 口径 + 共享头复用）、
> 58e335c4（测试计划文档）。

## 1. aclrtMemAttr 枚举对齐（P1）

- [x] 1.1 `dl_acl_api.h` 枚举改为与 CANN acl_rt.h 严格对齐的全量 18 项（0~17），注释错位风险与实测教训（commit e78a6faa；实机原始值直测：值 4 三档全成、值 16 超 4088MB 返 207001、值 7 恒返 100000）

## 2. P2P_HUGE→HBM_HUGE 回退（P2）

- [x] 2.1 `hybm_acl_vmm_segment` 新增 `MallocPhysicalWithFallback`：P2P_HUGE 失败且 ret==207001 → WARN（rankId/size/ret）→ HBM_HUGE 重试一次；`FillMemProp` 加 memAttr 入参；范围注释（仅 HBM 池、仅 die 间、卡间 BAR 限制 <4G/die）（commit e78a6faa）
- [x] 2.2 `acl_stub` 新增 `g_stubMallocP2pOom` 注入（P2P attr=15/16 返 207001）；UT 新增 `AllocPhysicalP2pOomFallbackToHbmHuge`（commit e78a6faa；`HybmAclVmmSegmentTest` 9/9、`DlAclApiVmmTest` 3/3 PASS）

## 3. benchmark/bm_310p 独立用例（P3）

- [x] 3.1 新增 `bm_perf_benchmark_310p`：双实体双池（entity0=HBM/SDMA 位、entity1=DRAM/HOST_SHM）、十方向按目的池路由 `HandleForRmt`、`-hbm-size/-dram-size`（默认 2GB、2M 对齐校验）、输出加 Pool 列；根 CMakeLists BUILD_TEST=ON 挂载（commit 7b548da7）
- [x] 3.2 `-hm pinned|pageable` 双口径：pinned=`aclrtMallocHost`+device 侧 copyType（L2G/G2L，SVM 窗口 VA 判 LOCAL_DEVICE），pageable=`posix_memalign`+host 侧（H2G/G2H）；输出 `host_mem` 标识；`FinalizeAll` 在 ResetDevice 前释放 pinned 缓冲（commit 7b548da7，原 dc5279ce；128MB 块 x16/x8 双口径 2×2 矩阵实测 WrongBytes 全 0）
- [x] 3.3 共享头复用：删除 `barrier_util.h`/`common_def.h` 本地副本（−254 行），CMake 两分支 include `../bm`，`band_width_calculator.h` 显式含 sstream；`benchmark/bm` 零改动（commit 7b548da7，原 dab24049；独立构建 + BUILD_TEST=ON 构建双验证）

## 4. 实机验证（P4）

- [x] 4.1 2GB 池（P2P 直配）与 8GB 池（回退 WARN 实证）十方向 × 2 rank 全 WrongBytes=0（dev4/dev5，测试计划附录 A.3）
- [x] 4.2 回归确认（共享头重构 + commit 重组后 HEAD 58e335c4 重建复跑）：十方向全 0，die 间 D2D 与基线一致（MR !3 评论 189308429）
- [x] 4.3 32GB 大池验证：dev4/dev5 干净 die 对，双 rank 回退 HBM_HUGE 建池 32GB，十方向全 0，退出后内存/共享内存零残留（会话实测 2026-09-11；教训：共享机器须先确认 die 对空闲）

## 5. 回归与文档（P5）

- [x] 5.1 UT：`HybmAclVmmSegmentTest` 9/9（含回退用例）、`DlAclApiVmmTest` 3/3、`HybmMemSegmentTest` 37/37（测试计划附录 A.3）
- [x] 5.2 文档：测试计划（F1-F3 裁决、M1 方案、附录 A/B/C 数据与复现命令、D.4/D.5 双口径实测）、能力矩阵 README、benchmark README（commit 58e335c4）
- [x] 5.3 后续决策登记：issue #10（是否默认切换 HBM_HUGE：天然实验/带宽无差异/910B 参照 vs 未文档化风险与卡间开放问题）
