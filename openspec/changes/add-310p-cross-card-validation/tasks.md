# Tasks: add-310p-cross-card-validation

## 1. 前置复核（实机）

- [ ] 1.1 运行 `310p-capability_matrix/12_topo_probe/run.sh` 复核本机拓扑与跨卡 P2P 带宽：8 die 基线、48 跨卡有序对 can=1 且带宽无数量级异常（排除上游机器 socket1 式劣化）；结果记入本 change 记录。验证方式：探针 PASS 且跨卡带宽数量级正常

## 2. D 态拆除产品化（池层）

- [ ] 2.1 `dl_acl_api.{h,cpp}` 新增 `aclrtDeviceDisablePeerAccess` 符号封装（typedef/静态成员/DL_LOAD_SYM/inline wrapper/unload 置空，镜像 Enable 五处）。验证方式：编译通过且符号缺失时返回 BM_UNDER_API_UNLOAD
- [ ] 2.2 `hybm_acl_vmm_segment.{h,cpp}` 跨卡 peer 跟踪与拆除：ImportOne 跨卡分支登记 slotAddr→peerDevId 与 peerDevId→计数；UnmapOne 成功路径递减、归零调用拆除（失败记 ERROR 不阻断）；UnReserve 兜底扫描剩余活跃对端。验证方式：代码评审 + UT 全绿
- [ ] 2.3 UT 新增 D 态拆除用例（stub so 加 DisablePeerAccess 计数符号）：跨卡全量释放恰好拆一次 / die 对不拆 / 部分释放不拆 / uninit 兜底拆；跑全量 UT 回归（含既有 HybmAclVmmSegmentTest 全部用例）。验证方式：新用例 PASS 且全量回归不低于基线

## 3. 示例与探针跨卡跑法

- [ ] 3.1 07 示例：`RANK_DEV` 改为读环境变量 `RANK_DEV_LIST`（缺省 "0,1" 保持 die 对），README 补跨卡运行段（`RANK_DEV_LIST=0,2`）。验证方式：缺省跑法与既有验收一致，跨卡跑法可启动
- [ ] 3.2 08 示例：`.asc` 的 devId 选择改为读环境变量 `MF_EX08_DEV_LIST`（缺省 "0,1"），README 补跨卡运行段（含两卡各自部署 kernel 的说明）。验证方式：同上
- [ ] 3.3 E0b 跨卡变体：从 git 017802a0 恢复 `probe_e0b.cc` 到未跟踪备份目录，dev 映射参数化（A_DEV/B_DEV），构建 probe_e0b_cc。验证方式：产物可执行且源码可对照原验收记录

## 4. 实机验收矩阵（跨卡 dev0↔dev2）

- [ ] 4.1 07 跨卡：`RANK_DEV_LIST=0,2` 双 rank 建池三阶段（HBM+DRAM 双池）逐字校验。验证方式：mismatch=0、FAIL 数=0、进程干净退出无 D 态
- [ ] 4.2 E0b 跨卡：probe_e0b_cc 双轮变体（本地 src→peer 池 GVA / own 池 GVA→peer 池 GVA）——SMMU 对跨卡导入 VA 的可译性裁决。验证方式：headAfter 前进且数据全对
- [ ] 4.3 08 跨卡端到端：两卡各自部署编排 kernel 后，`MF_EX08_DEV_LIST=0,2` 三阶段（mode13 闭环 / 串行×8 / mode14 流水）。验证方式：mismatch=0、双侧 mainRet=0、流水时延 < 串行对照、干净退出

## 5. 规格与文档同步

- [ ] 5.1 实测结论回填：`docs/310p/平台事实.md`（跨卡实测条目）、`docs/310p/部署档案.md`（两卡部署纪律）、`docs/310p/机制档案.md`（如跨卡通知链/带宽结论有新证据）；`310p-aicore-aicpu-开发进展.md` TODO-2 标注去向。验证方式：交叉引用路径全部存在
- [ ] 5.2 `openspec validate "add-310p-cross-card-validation" --strict` 通过且规格 delta 与实测结论一致（验收数字落 scenario 佐证或 change 记录）。验证方式：strict PASS
