# Design: add-310p-cross-card-validation

## Context

上游 xcclpp 协同模式的跨卡验证基于 demo 自分配内存（P2P policy + 跨卡 import 在 demo 脚本内闭环）；本仓把内存面换成 AclVmm 池 GVA 后，"跨卡导入 VA 是否可作 SQE src/dst"是上游没替我们验过的最大未知点（E0b 跨卡变体存在的意义）。同时跨卡导入隐式建立的 P2P 状态在产品清理路径中无拆除动作（D 态挂死风险，探针 03/04/10/14 四次实证）。

## Goals / Non-Goals

- Goals：D 态拆除进产品代码；跨卡验收矩阵推进到 08 端到端；两份 spec 消除超卖。
- Non-Goals：8-rank 跨卡（P2P 槽位预算另行立项）；跨节点（双机）场景；Reduce SQE / CQE / host 纯搬运（既有 TODO-0/4/5 不动）；OPP 产品化部署（TODO-1 不动）。

## Decisions

| 编号 | 决策 | 理由 |
|---|---|---|
| D1 | D 态拆除挂在**段内**：`HybmAclVmmSegment` 维护跨卡 peer 计数，`UnmapOne` 成功路径递减、归零即拆；`UnReserve` 兜底扫描剩余 | P2P 状态生命周期与导入句柄严格一一对应，段是唯一知情者；全局 uninit 钩子拿不到 rank→devId 映射 |
| D2 | 拆除走 `DlAclApi::AclrtDeviceDisablePeerAccess`（镜像既有 Enable 符号封装：typedef + DL_LOAD_SYM + inline wrapper + unload 置空） | 不引入 rt 内部符号，dlopen 失败安全降级（符号缺失返回 BM_UNDER_API_UNLOAD 并记 ERROR） |
| D3 | 07/08 的 rank→devId 映射用环境变量参数化（`RANK_DEV_LIST`），缺省值保持 dev0/dev1 die 对 | 向后兼容：既有 README 命令与验收记录零变化；跨卡是显式 opt-in |
| D4 | E0b 跨卡变体从 git 历史 017802a0 恢复 `probe_e0b.cc`，仅参数化 dev 映射，产物留在未跟踪备份目录 | 探针是过程产物不进产品 PR（既有纪律）；恢复而非重写保证与 E0b 验收记录可对照 |
| D5 | 验收顺序 topo → 07 → E0b → 08 | 先排除机器级 P2P 劣化（上游机器 socket1 曾劣化 80×，报障平台侧），再逐层上收：池建链 → SMMU 可译性 → 端到端 |
| D6 | 跨卡运行前两卡各自跑 deployer（作用域=卡，X9 实测跨卡文件系统独立） | 08 协同 kernel 部署粒度是卡；同卡单次部署的结论不覆盖跨卡 |

## Risks / Trade-offs

- [本机跨卡 P2P 链路劣化] → topo 探针复核门（步骤 0），带宽异常即止并按上游先例报障，不在坏链路上排障。
- [P2P 槽位预算] → 跨卡 import 隐式消耗槽位；2-rank 每对端 1 槽远低于上限（8-rank 受限是既有结论，本变更不碰）。
- [F6 跨卡导出必须 P2P policy] → 已满足：`FillMemProp` 固定 `ACL_MEM_P2P_HUGE`（产品代码现状，无需改动）。
- [DisablePeerAccess 失败阻断清理] → 拆除失败仅记 ERROR 不返回错误（清理路径必须推进，不能因拆不掉卡死其余 unmap）。

## 阻塞项：内核跨卡 import 的源 VA 映射约束（drvRetCode 17）

实机排障（9/14，dev0↔dev2）收敛出**内核级约束**，阻塞 4.1-4.3 全部产品链路验收：

**现象**：产品对称槽位布局（各 rank 物理映射于 `base + rank*maxSize`，对端槽位预留空置）下，
跨卡 `aclrtMemImportFromShareableHandle` 被驱动拒绝：`halMemImportFromShareableHandle drvRetCode=17`（EEXIST 语义），ACL 层表现为 507899。

**证据链**（cc_probe2 探针，多变体同进程对照）：
1. 手工链（双方各自 reserve 64MB@0x12c140000000 + malloc+map@win+0 + export）→ 跨卡双向 import + 数据校验全通（模式 0/9/11/13，≥3 次复现；plog 确认 ACL 5 步调用序列与实体链逐项相同）。
2. 实体链（产品段代码，r1 物理映射于 win+32MB 即对端槽位空置）→ 同参调用下 import 必败（模式 7/12）。
3. 同进程双句柄对照（模式 12）：实体句柄 FAIL、手工句柄 OK——排除时序/环境/堆踩踏（valgrind 零 Invalid write）。
4. 预热实验（模式 15）：import 前在对端槽位 VA 临时映射一块**新**物理 → 仍 FAIL——规则进一步收窄为「源 VA 须映射着**导入方自己导出的物理**」。
5. runtime plog 证实 auto-P2P（EnableP2PWithOtherDevice 双向）成功，失败点在 import ioctl 本身；借导出方 ctx 的方式三同样失败——排除 P2P 使能问题。

**进一步收敛（9/14 深夜补测，探针矩阵完备化后修正）**：上述「源 VA 映射约束」推断**被新证据推翻**——cc_probe3/4 系列（产品同款槽位布局 + NULL expect + 直调链，含完整 hybm_init/create_entity 前导）跨卡 import 双向全通（≥8 次复现）。同时 plog 对比证明：通过路径（直调链）与失败路径（产品段代码链）在**全部可观测层面逐字节一致**（ACL 调用序列、halMem 参数含 Reserve/Map/export、内核 devmm 日志、InfoGet 成功、句柄值传递正确）。已逐一排除并失败的修复假设：显式 expectPtr（addr 字段差异）、预热源 VA（未导出物理/已导出句柄两种）、借导出方 ctx（方式三）、登记阶段双向 enable（已合入，正确但非充分）。**结论修正：存在一个产品段代码路径特有的、当前仪器（plog/gdb/valgrind/strace）不可见的隐藏差异**，使内核 halMemImportFromShareableHandle 返回 drvRetCode=17；设备侧 devmm agent 源码（AICPU 固件）不在本机，宿主侧源码已排查完毕。

**解锁路径**（二选一，需团队决策）：
- **A. CANN/驱动侧澄清**：向驱动团队提交证据链，确认 drvRetCode 17 是否驱动缺陷（合法 import 被拒）；若修复则产品零改动。
- **B. 布局重设计**：2-rank 场景改为「own 物理@base+0、对端导入@base+maxSize」（探针已验证此布局内核接受），但破坏 GVA==DVA==HVA 对称不变式，牵动 AddVaInfo/TransformVa/peer 寻址全链——需独立 change 立项设计。

## 探索资产与证据矩阵（跨卡 import 排障结晶，索引）

**探针资产**（`310p-capability_matrix/17_aicore_aicpu_overlap/`，未跟踪目录，随备份保留）：
- `cc_probe.c` / `cc_probe2.cc`：初代裁决探针（模式 0-13：直调/段链/ctx技巧/预热/双句柄对照等变体矩阵）
- `cc_probe3.c` / `cc_probe4.cc`：终代裁决探针（产品槽位布局 + PRE 前导隔离 + ctrl/ctrlnul/fix 三模式）
- `hybm_min.cc`：最小 hybm 直调复现器（二分定位用）

**实验矩阵结论**（dev0↔dev2，各 ≥2 次复现）：

| 配置 | import 结果 |
|---|---|
| 产品槽位布局 + NULL expect + 直调链（含完整 hybm_init/create_entity 前导） | **PASS** |
| 产品槽位布局 + 显式 expectPtr + 直调链 | PASS |
| 产品槽位布局 + NULL/显式 expect + **产品段代码链**（07 示例 / cc_probe2 模式 7/12） | **FAIL 17** |
| 段链 + 预热源VA（未导出物理 / 已导出句柄） | FAIL / PASS(但直调链本就 PASS，预热非必要) |
| 借导出方 ctx（方式三）+ 段链 / 直调链 | FAIL / PASS |
| 13_bw 布局（单块窗口，import 先于 reserve） | PASS |
| xcclpp demo 的 IPC 链路跨卡 | PASS（demo + 03/13/15 号探针） |

**关键谜团**：直调链与段链在全部可观测层面逐字节一致（ACL 调用序列、halMem 参数、plog 内核日志、
句柄值传递、valgrind 零堆错误、单 libascendcl 实例），结果一 PASS 一 FAIL。宿主侧源码
（runtime api_impl / driver devmm_svm）已排查完毕，17 出自设备侧 AICPU devmm agent（固件源码不在本机）。

**已排除假设全录**：P2P 未使能／线程设备上下文／内核残留脏退出／双 ACL 实例／时序竞争／
源 VA 映射约束（被同布局直调 PASS 推翻）／显式 expectPtr addr 字段／源 VA 预热／
Disable 签名与设备绑定（后两项为已合入的正确修复，但非充分条件）。

**新分支路线（IPC 切换，方案文档 §7 已备案的兜底）**：
- 传统 IPC 的 import VA 驱动分配不可控（方案文档 619-620 行）→ GVA 对称公式（`base+p*maxSize`）
  改为地址查表（R1 兜底条款："回退 IPC+查表"）
- xcclpp demo 先例：SDMA SQE dst 用 IPC 导入 VA 跨卡可译 → E0b 结论对 IPC 内存需重验
- die 对路径保留 VMM（已验收），仅跨卡导入分支切换 → `IsDiePair` 分派点天然就是切换点

## 顺带发现（独立缺陷，另行修复）

- **exit 时静态析构顺序缺陷**：进程未显式 `hybm_uninit` 而经 `exit()` 退出时，`MemEntityFactory` 静态析构触发段拆除，调用已析构的 `HybmVaManager::RemoveOneVaInfo` → double-free 崩溃（valgrind/gdb 栈实锤，crash 栈：MemEntityFactory::~ → UnReserveMemorySpace → ReleaseSliceMemory → RemoveOneVaInfo）。die 对既有场景显式 uninit 不触发；脏退出路径必现。建议后续 change 修复（析构顺序守护或工厂空转检查）。
- **产品 join 失败清理路径堆异常**（malloc_consolidate/double-free，与上项同源或相邻）——随上项一并排查。

## Revision Impact

- `dl_acl_api.{h,cpp}`：+1 符号（typedef/成员/加载/封装/置空五处镜像 Enable；**实测 Disable 为单参数签名，Enable 为双参数**，typedef 已按真实签名修正）。
- `hybm_acl_vmm_segment.{h,cpp}`：+3 个私有容器（slotAddr→peerDevId、peerDevId→活跃计数、已 enable 对端集合），Import 登记阶段显式双向 enable（15 号探针配方），ImportOne 跨卡分支保持本进程 ctx import，UnmapOne 成功递减、归零拆除，UnReserve 兜底扫描。
- `hybm_entity_default.cpp`：Mmap/Unmap 入口补 SetThreadAclDevice（join 工作线程设备绑定——实测缺绑定时跨卡 enable/import 在错误设备上下文执行）。
- UT：stub so 增加 DisablePeerAccess 计数符号，新增跨卡拆除 4 用例。
- 示例 07/08：dev 映射读环境变量 + README 跨卡段。
