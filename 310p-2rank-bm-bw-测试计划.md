# 310P 2-rank BM 场景 bm_perf_benchmark 带宽测试改造计划（V1）

> **状态：已完成并实机验收通过（2026-09-09）**
> - 核心回退 + 枚举修复 + `benchmark/bm_310p` 独立用例全部落地
> - 实机：2-rank dev4/dev5 十方向 × {2MB, 16MB} 全部 WrongBytes=0；8GB 池回退链路 WARN 实证触发
> - UT：HybmAclVmmSegmentTest 9/9（含新增回退用例）、DlAclApiVmmTest 3/3、HybmMemSegmentTest 37/37
> - 实施记录与带宽数据见文末附录 A/B/C
>
> 目标：让 `benchmark/bm/bm_perf_benchmark`（`-bw` 带宽模式）在 **Atlas 300I Duo（Ascend310P3）
> 单卡双 die 2-rank** 场景下可运行，产出 10 个方向的 BM 带宽数据（H2D/D2H/H2RD/H2RH/D2RD/D2RH/
> RH2D/RH2H/RD2D/RD2H）。
>
> 范围约束：**只考虑 2-rank 310P 的 BM 内存池**（HBM ACL VMM + DRAM HOST_SHM 双池），
> 不考虑多节点/其他 op 类型/910B 回归以外的场景。
>
> 上游文档：
> - `310p-2rank-vmm-开发计划.md`（双池能力实机验收：07 示例 3/3 轮全绿）
> - `/home/t00842504/310p_probe/0824-test/05_vmm_die/README.md`（★4GB/P2P 分区问题实证）

---

## 一、结论先行

**当前 bm_perf_benchmark 不能直接跑 2-rank 310P 带宽，需要修改后才能测。**
核心原因 3 条（均已实测或代码证实）：

1. **4GB 物理分配必失败**：benchmark 硬编码 `GVA_SIZE = 4GB`（benchmark/bm/band_width_calculator.cpp:24），
   而 310P HBM 池物理分配硬编码 `ACL_MEM_P2P_HUGE`（src/hybm/csrc/mm/hybm_acl_vmm_segment.cpp:123），
   P2P 分区实测上限 **4088MB**（动态 4083~4091MB），4096MB 必返 207001。
2. **单实体双池不可行**：benchmark 用单实体同时建 HBM+DRAM 池（band_width_calculator.cpp:433），
   310P 上 DRAM 段分派到 `HybmConnBasedSegment`，其 `CheckSdmaReaches` 用基类实现恒 false
   （hybm_mem_segment.cpp:114），`InitDramSegment` 直接失败（hybm_entity_default.cpp:1213）。
   **本机已实测复现**（`-ot sdma` 报 `dram segment does not support sdma in current environment`）。
3. **HOST_SHM 强制独立实体**：benchmark 的 `ParseOpType` 不含 `host_shm`；且 HOST_SHM 禁止
   与 HBM 同实体、禁止与其他 op 混用（src/smem/csrc/smem_bm/smem_bm.cpp:218-231）。
   310P 正确姿势是 **双实体**（07 示例已实机验证）。

---

## 二、已知事实（F 系列，全部实测/代码证实）

| # | 事实 | 证据 | 影响 |
|---|------|------|------|
| F1 | **P2P 分区上限 ~4088MB**：`MallocPhysical(P2P_HUGE/P2P_NORMAL)` 超 4088MB 返 207001（分区 OOM）；上限随设备空闲动态波动（实测 4083~4091MB） | 05_vmm_die 二分法实测（2MB 粒度） | benchmark HBM 池 ≤ 2GB 可避开；>4GB 必须换 attr |
| F2 | **`HBM_HUGE` 无 P2P 限制**：从 AI 主池（~42GB）分配，4GB 全链路 PASS，带宽与 P2P_HUGE 无差异（读 19.00 / 写 17.93 GB/s） | 05_vmm_die 大块轮实测 | 核心 fallback 的依据；探针结论"die 对场景 HBM_HUGE 优于 P2P_HUGE" |
| F3 | `HybmAclVmmSegment::FillMemProp` 硬编码 `ACL_MEM_P2P_HUGE`，无回退 | hybm_acl_vmm_segment.cpp:119-126 | 4GB 池必失败；需 fallback 或缩池 |
| F4 | die 对 `EnablePeerAccess` 返 507899（同 BDF 非法 P2P 对）；import 必须走 ctx 技巧 —— **HEAD 的 AclVmmSegment 已内置**（五步 SetContext 导入） | 05_vmm_die + 已合入代码 | benchmark 无需关心，走 smem_bm API 即可 |
| F5 | 310P 上 `-ot sdma` 单实体双池：DRAM 段 ConnBased → `CheckSdmaReaches` 基类恒 false → 建实体失败 | 本机实测日志 + hybm_mem_segment.cpp:114 | 单实体路线不可行，必须双实体 |
| F6 | HOST_SHM 实体约束：`localDRAMSize>0 && localHBMSize==0`，禁与其他 op 位混用 | smem_bm.cpp:218-231 | DRAM 池必须独立 entity |
| F7 | **方向集合恰好二分**（代码证实）：<br>• HBM 实体（SDMA op 位→compose→`HostDataOpAclMemcpy`，占 SDMA 槽）：`L2G/G2L/G2G/H2G/G2H` 共 5 个 DEVICE-global 方向；D2D 走 async 批量提交+末尾单 Sync，H2D/D2H 同步；batch 全支持<br>• DRAM 实体（HOST_SHM→`HostDataOpHostShm`）：`H2GH/GH2H/G2G(host)/L2GH/GH2L` 共 5 个 HOST-global 方向；全同步（不支持 async）；batch 全支持 | hybm_data_op_acl_memcpy.cpp:48-60、hybm_data_op_host_shm.cpp:41-53 | 10 方向 = 5(HBM 实体) + 5(DRAM 实体)，无缺口 |
| F8 | **copyType 逻辑无需改**：`TransToHybmDirection` 按**地址分类** dst/src（对该实体的 GVA 窗口），copyType 仅钉住 local 侧类型。现有 `L2G/H2G/G2L/G2H` 在双实体下语义自动落位（如 DRAM 实体上 `L2G` + dst=host 池 → `LOCAL_DEVICE_TO_GLOBAL_HOST`） | smem_bm_entry.cpp:598-637 directMap | benchmark 只需**按方向路由 handle**，copyType 选择逻辑保持 |
| F9 | 跨实体地址互斥：在 HBM 实体上引用 DRAM 池地址（或反之），`GetHybmMemTypeFromGva` 返 BUTT → `SM_INVALID_PARAM` | smem_bm_entry.cpp:580-589 | 每方向必须用对实体，路由错误会显式报错（可探测） |
| F10 | `smem_bm_ptr_by_mem_type(handle, HOST/DEVICE, rank)` 按 `maxDRAMSize/maxHBMSize × rank` 偏移取池基址，对两个实体各自可用 | smem_bm.cpp:419-448 | 双实体下取地址 API 不变，只是 handle 换 |
| F11 | **output/ 下的 so 是 2026-08-30 构建，早于 09-01 的 310p 合入**（HEAD=54eb5341） | 文件时间戳 + git log | 任何实测前必须从 HEAD 重建核心库 |
| F12 | join 为动态成员语义：先 join 者需等全员 join + 增量导入生效后再拷贝 | 07 示例注释（F19） | benchmark 已有 join 后 barrier，保持 |
| F13 | 本机 8 dev = 4 卡×2 die；实测时 dev4/dev5 或 dev6/dev7 常空闲（npu-smi 以实时为准） | npu-smi info | `-d` 指定空闲 die 对；HBM 池大小受设备空闲影响（F1 动态上限） |

---

## 三、差距分析：benchmark 现状 vs 310P 2-rank 需求

| # | benchmark 现状（代码位置） | 310P 2-rank 需要 | 差距 |
|---|---------------------------|------------------|------|
| G1 | 单实体 `smem_bm_create(0, 0, opType, 4GB, 4GB, flags)`（band_width_calculator.cpp:433） | 双实体：entity0=HBM 池（SDMA 位，dram=0）+ entity1=DRAM 池（HOST_SHM，hbm=0），池大小可配（默认 2GB） | 改建池逻辑 |
| G2 | 池大小硬编码 `GVA_SIZE=4GB`（:24） | HBM ≤2GB 避开 P2P 上限；或核心 fallback 后可 4GB | 改默认值 + 加 CLI |
| G3 | 方向函数假设同一 handle 提供 HOST/DEVICE 两池地址（`PrepareCopyParam` :319、`CheckCopyResult` :350） | 按方向路由：`rmtMemType==DEVICE`→HBM 实体；`==HOST`→DRAM 实体 | 加路由 |
| G4 | `PrepareLocalMem` 的 SDMA 分支对 HOST 池直接 `memcpy` 预填（:295-300），handle 为单实体 | 预填改在 DRAM 实体的 own host 指针上（HOST_SHM 池是真实 host 内存，可直接 memcpy；HBM 池 VMM VA **不可** CPU 解引用——F14/上游） | 改预填目标 |
| G5 | `ParseOpType` 无 `host_shm`（main.cpp:112-138） | 双池模式下 DRAM 实体固定用 `SMEMB_DATA_OP_HOST_SHM`（内部值，非用户 CLI 语义） | CLI 设计（见 D2） |
| G6 | `-ot` 其余类型（rdma/urma/uboe/tcp）在 310P 上不可用/不在范围 | 不动，保持原路径（非 310P 双池模式走原逻辑） | 零改动 |
| G7 | 销毁/收尾单实体（`smem_bm_destroy(handle)` :459） | 双实体按逆序销毁（DRAM 后建先毁，对齐 07 示例） | 改收尾 |

---

## 四、设计核心：方向 × 实体 × 算子映射矩阵

10 个方向到双实体的完整路由（F7/F8 代码证实；`all` 模式按原顺序依次执行）：

| benchmark 方向 | 源 | 目的（池） | 执行实体 | hybm 方向 | 底层算子 |
|---|---|---|---|---|---|
| H2D  | localDram_(host malloc) | own HBM 池 | **HBM** | LOCAL_HOST_TO_GLOBAL_DEVICE | AclrtMemcpy 同步 H2D |
| D2H  | localHbm_(aclrtMalloc) | own DRAM 池 | **DRAM** | LOCAL_DEVICE_TO_GLOBAL_HOST | aclrtMemcpy 同步 D2H |
| H2RD | localDram_ | peer HBM 池 | **HBM** | LOCAL_HOST_TO_GLOBAL_DEVICE | 同步 H2D（跨 die） |
| H2RH | localDram_ | peer DRAM 池 | **DRAM** | LOCAL_HOST_TO_GLOBAL_HOST | host memcpy |
| D2RD | localHbm_ | peer HBM 池 | **HBM** | LOCAL_DEVICE_TO_GLOBAL_DEVICE | AclrtMemcpyAsync D2D + Sync |
| D2RH | localHbm_ | peer DRAM 池 | **DRAM** | LOCAL_DEVICE_TO_GLOBAL_HOST | 同步 D2H |
| RH2D | peer DRAM 池 | localHbm_ | **DRAM** | GLOBAL_HOST_TO_LOCAL_DEVICE | 同步 H2D |
| RH2H | peer DRAM 池 | localDram_ | **DRAM** | GLOBAL_HOST_TO_LOCAL_HOST | host memcpy |
| RD2D | peer HBM 池 | localHbm_ | **HBM** | GLOBAL_DEVICE_TO_LOCAL_DEVICE | async D2D + Sync |
| RD2H | peer HBM 池 | localDram_ | **HBM** | GLOBAL_DEVICE_TO_LOCAL_HOST | 同步 D2H |

要点：
- 路由规则一条：**目的池类型决定实体**（DEVICE→entity0，HOST→entity1）。
- copyType 选择逻辑（Put: L2G/H2G；Get: G2L/G2H）**保持不变**（F8）。
- `CheckCopyResult` 的回读 G2H 同样在对应实体上发（HBM 实体 G2H / DRAM 实体 GH2H 语义由地址分类自动落位）。
- 方向执行顺序保持原序（H2D 先于 RD2D 等，保证池内容确定性：peer HBM 池内容由 peer 的 H2D 写入，
  peer DRAM 池内容由 peer 的 D2H/H2RH 写入）。

---

## 五、探测阶段（P0，编码前，预计 0.5 天）

> 目的：消除"代码读通但实机未走过"的残余风险；产出物记入本文件附录。
> 前置：从 HEAD 重建核心库（F11）+ benchmark 链接新库。

| # | 探测项 | 方法 | 通过标准 |
|---|--------|------|----------|
| P1 | 重建核心库 | `bash script/build_and_pack_run.sh`（RELEASE/NPU/Python ON），benchmark 重新编译 | 编译通过，so 时间戳更新 |
| P2 | 空闲 die 对确认 + P2P 余量 | `npu-smi info` 选 die 对；05 号探针口径确认该 dev P2P free | P2P free ≥ 拟用 HBM 池大小（2GB） |
| P3 | **2GB HBM 池 + 2GB DRAM 池双实体 join** | 临时小程序或改 07 示例参数（32MB→2GB）跑一轮 | 双实体 join OK、互相可见、拷贝校验 mismatch=0 |
| P4 | **batch + raw host ptr on HBM 实体**（H2D/H2RD 口径） | `smem_bm_copy_batch` H2G：src=未注册 host malloc、dst=own/peer HBM 池 | 07 示例只测过单条 copy；batch 接口 + 未注册 host 源需实证 |
| P5 | **raw device ptr on DRAM 实体**（D2H/D2RH/RH2D 口径） | `smem_bm_copy_batch` L2GH/GH2L：src/dst=裸 aclrtMalloc 设备指针 | 示例未覆盖的 L2GH/GH2L 方向实证 |
| P6 | barrier 端口可用性 | benchmark BarrierUtil 用 `-ip` 端口+12664 偏移（默认 21666） | 端口无冲突 |
| P7 | （仅当采用 M1-B 核心 fallback）4GB 池 join | 修改后 4GB HBM 池走 HBM_HUGE | join OK（05 探针已单证 ACL 层可行，需经 smem_bm 全链复证） |

> P4/P5 若失败：fallback 方案为 benchmark 侧把裸指针改为 `smem_bm_register_user_mem` 显式注册
> （HBM 实体注册 device 指针、DRAM 实体注册 host 指针，best-effort WARN 不致命的原逻辑保留）。

---

## 六、修改方案

### M1（可选增强）：核心 HBM_HUGE 回退 —— `src/hybm/csrc/mm/hybm_acl_vmm_segment.cpp`

> **方案 A（默认，零核心改动）**：benchmark 池默认 2GB，完全避开 F1/F3。
> **方案 B（本节，增强）**：P2P_HUGE 失败（207001）时自动回退 HBM_HUGE 重试，支持 ≥4GB 池。
> 建议：**先 A 后 B**。A 当天可出数据；B 作为独立小改动（约 15 行 + UT），不影响 A 验证。

- `FillMemProp` 加 `aclrtMemAttr` 入参或成员；
- `AllocLocalMemory`：`AclrtMallocPhysical(P2P_HUGE)` 失败且 ret==207001 → WARN 日志（含 rankId/size/ret）
  → 换 `ACL_MEM_HUGE` 重试一次；成功后 INFO 记录实际 attr；
- 依据：F2（HBM_HUGE 4GB 实测 PASS、带宽无差异、AI 主池 42GB 充裕）；
- UT：`HybmAclVmmSegmentTest` 补 fallback 用例（mock AclrtMallocPhysical 首败 207001 次成）。

### M2（主体）：benchmark 双池改造 —— `benchmark/bm/`

**M2.1 参数与模式（main.cpp / band_width_calculator.h）**
- `BwTestParam` 增加：`hbmPoolSize`（默认 2GB）、`dramPoolSize`（默认 2GB）、`dualPool`（模式开关）；
- CLI 增加 `-hbm-size/-dram-size`（字节）；
- 模式启用（见 D2 决策点）：倾向 **自动检测**——`-ot sdma` 时经 `aclrtGetDeviceSocVersion` 识别
  ASCEND310Px → dualPool=true；保留 `-dp/--dual-pool` 显式开关覆盖自动判定；
- 参数校验：`batchSize × copySize ≤ min(hbmPoolSize, dramPoolSize)`（不足则 ERROR 退出，带关键参数）。

**M2.2 双实体生命周期（band_width_calculator.cpp Execute，:419-462）**
- dualPool 时：
  - `hbmHandle = smem_bm_create(0, 0, SDMA, 0, hbmPoolSize, flags)`（**dram=0**）
  - `dramHandle = smem_bm_create(1, 0, HOST_SHM, dramPoolSize, 0, 0)`（**hbm=0，flags 清 SDMA 专属位**）
  - 两个 handle 各自 `smem_bm_join`，join 后沿用现有 barrier；
  - 销毁逆序：先 dramHandle 后 hbmHandle（对齐 07 示例）。
- 非 dualPool：原单实体路径原样保留（910B 等场景零影响）。

**M2.3 方向路由（PrepareCopyParam / BatchCopyPut / BatchCopyGet / CheckCopyResult）**
- 新增成员 `smem_bm_t handleForRmt(smem_bm_mem_type rmtMemType)`：DEVICE→hbmHandle_，HOST→dramHandle_；
- 上述四处取池地址/发拷贝时统一走该路由（copyType 逻辑不动，F8）；
- 错误路径按规范记 ERROR 日志（含 rankId、方向名、rmtMemType、ret）。

**M2.4 PrepareLocalMem 调整（:265-303）**
- SDMA 分支的 HOST 池预填 `memcpy`（:295-300）改为在 **dramHandle_** 的 own host 指针上执行；
- `localDram_`（host）注册：dualPool 下注册到 dramHandle_（best-effort，WARN 不致命）；
- `localHbm_`（device）注册：dualPool 下注册到 hbmHandle_（device 注册为纯 VA 登记，310P 可用）；
  原"SDMA 跳过 HBM 注册"的 910 逻辑仅在非 dualPool 生效；
- HBM 池地址**严禁** CPU 直接解引用（上游 F14），预填只允许打 DRAM 池。

**M2.5 结果与输出**
- 结果表头增加 `Pool` 列（HBM/DRAM），标注每方向所属实体；
- README 补 310P 2-rank 用法章节：前置条件、命令示例、P2P 上限说明（F1）、带宽参考值。

**M2.6 函数行数/规范约束**
- `Execute`/`PrepareLocalMem` 改造后若超 50 行，按"建池/join/销毁"拆独立函数；
- 所有新增错误返回路径记 ERROR 日志（rankId、handle 归属、size、ret）。

### M3：构建与验证

1. 重建核心（M1-B 若做）：`bash script/build_and_pack_run.sh`；
2. benchmark 编译：`benchmark/bm/` 下 `cmake . -B build && make -C build`（对齐 README）；
3. **正确性冒烟**（dev 对空闲时）：
   `./bm_perf_benchmark -bw -ot sdma -t all -s 1M -bs 1 -cc 10 -d <dev> -ws 2 -lrs 2 -ip tcp://127.0.0.1:<port>`
   → 10 方向全部出现且 `WrongBytes=0`；
4. **正式带宽采集**：`-s 2097152 -cc 100`（对齐 README 口径）；可选尺寸扫描（64KB/512KB/1M/2M/16M）；
5. 数据记录：结果表 + 环境信息（dev 对、固件版本、池大小）追加到本文件附录。

---

## 七、决策点（待审阅拍板）

| # | 决策点 | 选项 | 建议 |
|---|--------|------|------|
| D1 | 4GB 问题处理 | A. benchmark 默认 2GB 池（零核心改动）；B. 核心 P2P→HBM_HUGE fallback；A+B | **A 先行出数据， B 作为独立增强**（05 探针已证 B 可行） |
| D2 | dualPool 启用方式 | a. soc 自动检测；b. 显式 `-dp` 开关；a+b 互备 | **a+b**：自动为主，显式可覆盖 |
| D3 | 池默认大小 | 1GB / 2GB / 4GB(需 M1-B) | **2GB**（P2P 动态上限 4083MB 的安全余量） |
| D4 | 带宽口径 | 仅 2MB×100；或加尺寸扫描 | 主口径 2MB×100，扫描可选 |
| D5 | `-ot` 其他类型在 310P 的行为 | 报错退出 / WARN 跳过 | 保持现状（不在本计划范围） |

## 八、风险与回退

| 风险 | 概率 | 缓解 |
|------|------|------|
| P4/P5 探测失败（batch/裸指针方向不被 compose/shm 算子接受） | 中 | 注册 fallback（P0 节末尾方案）；仍失败则该方向标 N/A 并记录原因 |
| P2P 上限随设备繁忙缩水（F1 动态） | 低 | 池 2GB 距 4083MB 下限余量足；报错日志含 size/ret 可定位 |
| 双实体 join 时序（F12 动态成员） | 低 | 已有 join 后 barrier；07 示例同模式已验证 |
| 核心重建引入回归（M1-B） | 低 | fallback 仅 310P AclVmm 路径新增分支；跑 `run_ut.sh --fast` 相关套件 |
| benchmark 改动影响 910B 原路径 | 低 | dualPool 分支隔离，非 dualPool 走原逻辑（可 grep 确认无交叉） |

## 九、交付物

1. M1-B（若采纳）：核心 fallback 代码 + UT 用例；
2. M2：benchmark 双池改造代码 + README 310P 章节；
3. 2-rank 310P 十方向带宽数据表（含环境信息）→ 追加至本文件附录；
4. 探测记录（P2-P7 结果）→ 本文件附录。

## 十、执行顺序（采纳后）

```
P1 重建 → P2/P3 探测（双池 join）→ P4/P5 探测（batch 方向）→ M2 benchmark 改造
→ M3.3 冒烟（10 方向 WrongBytes=0）→ M3.4 正式采集 → （并行可选）M1-B + P7 + UT
→ 附录记录 → 完成
```

预计工作量：探测 0.5 天 + M2 改造 0.5~1 天 + 验证采集 0.5 天（M1-B 另计 0.5 天）。

---

## 附录 A：实施记录（2026-09-09）

### A.1 关键新发现：dl_acl_api.h 枚举与 CANN 错位（修正 F3 前提）

实机实证（mini 探针 `attr_probe.c`，dev4，aclrtMallocPhysical 原始值直测）：

| 原始值 | CANN acl_rt.h 语义 | 32MB | 4GB | 8GB |
|--------|-------------------|------|-----|-----|
| 4 | `ACL_HBM_MEM_HUGE`（AI 主池 ~42GB） | OK | OK | OK |
| 16 | `ACL_MEM_P2P_HUGE`（P2P 分区 ~4091MB） | OK | **207001** | **207001** |
| 7 | `ACL_DDR_MEM_P2P_NORMAL` | 100000 | 100000 | 100000 |

- **原 dl_acl_api.h 自编号枚举**（`ACL_MEM_P2P_HUGE=4`、`ACL_HBM_MEM_HUGE=7`）与 CANN 真实
  aclrtMemAttr（0~17）不一致：合入前的代码"名为 P2P_HUGE 实发 HBM_HUGE(4)"——**意外避开了
  4088MB 上限**（这也解释了 F3 前提下 4GB 池为何从未失败过）。
- **修复**：dl_acl_api.h 枚举改为与 CANN 严格对齐（0~17 全量，带注释）。此后
  `ACL_MEM_P2P_HUGE`=16 语义为真 P2P（受 ~4091MB 分区限制），`ACL_HBM_MEM_HUGE`=4 为
  42GB 主池——回退链路的两个 attr 才真正名实相符。

### A.2 代码变更清单

| 文件 | 变更 |
|------|------|
| `src/hybm/csrc/under_api/dl_acl_api.h` | aclrtMemAttr 对齐 CANN（0~17） |
| `src/hybm/csrc/mm/hybm_acl_vmm_segment.{h,cpp}` | `MallocPhysicalWithFallback`：P2P_HUGE 失败(207001)→WARN(含 rankId/size/ret)→HBM_HUGE 重试；`FillMemProp` 加 attr 入参 |
| `test/ut/mock/acl_stub.cpp` | 新增 `g_stubMallocP2pOom` 注入（P2P attr=15/16 时返 207001） |
| `test/ut/testcase/hybm/mm/hybm_acl_vmm_segment_test.cpp` | 新增 `AllocPhysicalP2pOomFallbackToHbmHuge` 用例 |
| `benchmark/bm_310p/`（新增，复制自 benchmark/bm 不改原版） | `bm_perf_benchmark_310p`：双实体（entity0=HBM/SDMA 位, entity1=DRAM/HOST_SHM）、按目的池路由 handle、`-hbm-size/-dram-size`（默认 2GB/2M 对齐校验）、`-ot/-rdma` 移除、结果表加 Pool 列 |
| `CMakeLists.txt`（根） | BUILD_TEST=ON 时挂 bm_310p 子目录 |

### A.3 验证结论

- **2GB 池**（P2P_HUGE 直配）：十方向 × 2 rank 全 WrongBytes=0
- **8GB 池**（强制触发回退）：双 rank WARN `AclrtMallocPhysical(P2P_HUGE) P2P partition exhausted,
  fallback to ACL_HBM_MEM_HUGE, ret:207001` 实证打印，十方向全 0
- **P2P vs HBM_HUGE 带宽**：16MB 块两轮（枚举修复前后，即 HBM_HUGE vs 真P2P_HUGE）D2RD 15.3/15.4、
  RD2D 16.4/16.4 GB/s——无差异，与 05 探针结论一致
- **UT**：HybmAclVmmSegmentTest 9/9、DlAclApiVmmTest 3/3、HybmMemSegmentTest 37/37
- **环境踩坑**：`run_ut.sh` 会以 ASAN 构建覆盖 `output/` 下的 so（非 ASAN 进程加载报
  "cannot open shared object file"）——跑 UT 后需重跑 `build_and_pack_run.sh` 恢复 RELEASE 产物

## 附录 B：2-rank 310P BM 带宽数据（2026-09-09 实测）

环境：Atlas 300I Duo（Ascend310P3）dev4/dev5 同卡 die 对，CANN 9.1.0，
HBM 池 2GB(P2P_HUGE) + DRAM 池 16MB(HOST_SHM, /dev/shm 仅 64MB)，
batch=1，数据为两 rank 各一行（取值接近，读表时按方向看两行即可）。

### B.1 块 2MB × 100 次（`-s 2097152 -cc 100`）

| 方向 | 池 | 算子路径 | BW rank0 (GB/s) | BW rank1 (GB/s) |
|------|-----|---------|----------------|----------------|
| H2D  | HBM | aclrtMemcpy H2D 同步 | 5.490 | 5.457 |
| D2H  | DRAM | aclrtMemcpy D2H 同步 | 5.905 | 5.876 |
| H2RD | HBM | aclrtMemcpy H2D 同步(跨 die) | 5.852 | 5.815 |
| H2RH | DRAM | host memcpy | 16.673 | 15.819 |
| D2RD | HBM | AclrtMemcpyAsync D2D + Sync | 10.198 | 10.112 |
| D2RH | DRAM | aclrtMemcpy D2H 同步(跨 die) | 5.981 | 6.016 |
| RH2D | DRAM | aclrtMemcpy H2D 同步(跨 die) | 6.095 | 6.131 |
| RH2H | DRAM | host memcpy | 17.167 | 17.163 |
| RD2D | HBM | AclrtMemcpyAsync D2D + Sync | 10.704 | 10.500 |
| RD2H | HBM | aclrtMemcpy D2H 同步(跨 die) | 5.962 | 5.926 |

### B.2 块 16MB × 30 次（`-s 16777216 -cc 30`）

| 方向 | 池 | BW rank0 (GB/s) | BW rank1 (GB/s) |
|------|-----|----------------|----------------|
| H2D  | HBM | 6.750 | 6.802 |
| D2H  | DRAM | 6.243 | 6.240 |
| H2RD | HBM | 6.810 | 6.831 |
| H2RH | DRAM | 13.582 | 13.888 |
| D2RD | HBM | 15.321 | 15.329 |
| D2RH | DRAM | 6.378 | 6.414 |
| RH2D | DRAM | 6.870 | 6.924 |
| RH2H | DRAM | 14.683 | 15.057 |
| RD2D | HBM | 16.360 | 16.375 |
| RD2H | HBM | 6.514 | 6.535 |

### B.3 数据解读

- **die 对 D2D（D2RD/RD2D）**：16MB 块 15.3~16.4 GB/s（2MB 块 10.1~10.7，块增大摊薄逐次
  Sync 开销）——与 05 探针 16MB 口径 19.0/17.9 GB/s 同量级（探针为 30 连发+末尾单 Sync，
  本工具每 batch(1 块)一次 Sync，口径更保守）
- **host memcpy（H2RH/RH2H）**：跨 rank DRAM 池纯内存拷贝 13.6~17.2 GB/s，为 host 侧上限
- **H2D/D2H 系（同步 aclrtMemcpy）**：5.5~6.9 GB/s，单次同步 DMA 提交+等待的固定开销主导，
  块从 2MB→16MB 仅小幅提升——同步语义下带不动更高吞吐
- 所有方向 WrongBytes=0（逐字校验通过）

## 附录 C：复现命令

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
export LD_LIBRARY_PATH=<repo>/output/smem/lib64:<repo>/output/hybm/lib64:$LD_LIBRARY_PATH

# 正式带宽(2MB): 十方向
./bm_perf_benchmark_310p -bw -t all -s 2097152 -cc 100 -d 4 -ws 2 -lrs 2 \
    -hbm-size 2147483648 -dram-size 16777216 -ip tcp://127.0.0.1:28570

# 回退链路验证(8GB HBM 池, 观察 WARN "P2P partition exhausted ... fallback")
./bm_perf_benchmark_310p -bw -t all -s 2097152 -cc 50 -d 4 -ws 2 -lrs 2 \
    -hbm-size 8589934592 -dram-size 16777216 -ip tcp://127.0.0.1:28570
```
