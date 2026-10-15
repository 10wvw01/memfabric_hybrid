# R3 补丁：stars_v2 补齐 sqcq_reg_map（开放仓可修版）

> 2026-10-15 第二轮核实产物。基线：gitcode.com/cann/driver 开源仓。
> **根因修正**：remote SQ 门铃查不到（prop=5 → 65534）的根因不是（只是）闭源注册
> handler 跳过——是 **stars_v2 适配层把整块寄存器相关 ops 全部置 NULL**
> （`trs_core_stars_v2_ops.c:119-124`），v1 有完整实现没被迁移。
> **安全性**：补丁失败=返回错误、uva 保持 0=现状；成功=解锁整条直驱链。

## 一、断链全景（每行已核实）

```
remote SQ 注册 (trs_hw_sqcq.c:606/:965, 开放)
  → trs_sqcq_reg_map (trs_res_mng.c:1074)
      :1077 if (ts_inst->ops.sqcq_reg_map != NULL)   ← stars_v2 = NULL!!
      （v1 走 ops.sqcq_reg_map → 发 TRS_MSG_SQ_REG_MAP 消息 → 固件回填 va
        → trs_set_sq_reg_vaddr 填 sq_ctx[sqid].reg_mem.uva）
  → v2: uva 恒 0
  → 查询: 用户态白名单(trs_sqcq.c:1482)不含 REG_BASE → ioctl 进内核
    → 内核 case 存在(trs_hw_sqcq.c:1668) → trs_query_sq_reg_vaddr(:1626)
    → 读 uva=0 → -EOPNOTSUPP → 65534            ← 断点唯一且精确

v1 参考实现（要抄的代码）: trs_core_stars_v1_ops.c:72-94 (reg_map) / :96-115 (reg_unmap)
消息机制: trs_host_msg_send (trs_host_msg.c:170) = devdrv_sync_msg_send 同步邮箱
          （发 TRS_MSG_SQ_REG_MAP + remote_tgid → 设备侧处理 → 回填 sq_map.va）
uva 存储: trs_set_sq_reg_vaddr (trs_hw_sqcq.c:1566, KA_EXPORT_SYMBOL_GPL)
```

## 二、Patch（唯一必需改动，1 个文件）

**目标文件**：`src/sdk_driver/trsdrv/trs/lba/near/comm/adapt/trs_host_core/stars_v2/trs_core_stars_v2_ops.c`

**改动 1 — include 区**（若尚未包含）：

```c
#include "trs_h2d_msg.h"   /* TRS_MSG_SQ_REG_MAP / trs_msg_sqcq_sync */
```

**改动 2 — 在 `trs_core_stars_v2_ops` 表定义之前新增两个函数**（照 v1
`trs_core_stars_v1_ops.c:72-115` 适配，函数名按 v2 命名惯例）：

```c
static int trs_core_stars_v2_ops_sqcq_reg_map(struct trs_id_inst *inst,
                                              struct trs_sqcq_reg_map_para *para)
{
    struct trs_msg_sqcq_sync *msg_sqcq_sync = NULL;
    struct trs_msg_data msg;
    int ret;

    msg.header.cmdtype = TRS_MSG_SQ_REG_MAP;
    msg.header.valid = TRS_MSG_SEND_MAGIC;
    msg.header.tsid = inst->tsid;
    msg_sqcq_sync = (struct trs_msg_sqcq_sync *)msg.payload;
    msg_sqcq_sync->remote_tgid = para->host_pid;
    msg_sqcq_sync->bind_sqcq.stream_id = para->stream_id;
    msg_sqcq_sync->sq_id = para->sqid;
    msg_sqcq_sync->cq_id = para->cqid;
    ret = trs_host_msg_send(inst->devid, &msg, sizeof(struct trs_msg_data));
    if ((ret != 0) && (ret != -ESRCH)) {
        trs_err("Msg send fail. (devid=%u; ret=%d)\n", inst->devid, ret);
        return ret;
    }
    (void)trs_set_sq_reg_vaddr(inst, para->sqid, msg_sqcq_sync->sq_map.va,
                               msg_sqcq_sync->sq_map.size);
    return ret;
}

static int trs_core_stars_v2_ops_sqcq_reg_unmap(struct trs_id_inst *inst,
                                                struct trs_sqcq_reg_map_para *para)
{
    struct trs_msg_sqcq_sync *msg_sqcq_sync = NULL;
    struct trs_msg_data msg;
    int ret;

    msg.header.cmdtype = TRS_MSG_SQ_REG_UNMAP;
    msg.header.valid = TRS_MSG_SEND_MAGIC;
    msg.header.tsid = inst->tsid;
    msg_sqcq_sync = (struct trs_msg_sqcq_sync *)msg.payload;
    msg_sqcq_sync->remote_tgid = para->host_pid;
    msg_sqcq_sync->bind_sqcq.stream_id = para->stream_id;
    msg_sqcq_sync->sq_id = para->sqid;
    msg_sqcq_sync->cq_id = para->cqid;
    ret = trs_host_msg_send(inst->devid, &msg, sizeof(struct trs_msg_data));
    if (ret != 0) {
        trs_err("Msg send fail. (devid=%u; ret=%d)\n", inst->devid, ret);
        return ret;
    }
    (void)trs_set_sq_reg_vaddr(inst, para->sqid, 0, 0);
    return ret;
}
```

**改动 3 — ops 表**（`:119-120`）：

```c
    .sqcq_reg_map = trs_core_stars_v2_ops_sqcq_reg_map,      /* 原 NULL */
    .sqcq_reg_unmap = trs_core_stars_v2_ops_sqcq_reg_unmap,  /* 原 NULL */
```

## 三、明确不用改的（第二轮核实修正）

| 位置 | 旧结论（§8.3/§13） | 修正 |
|---|---|---|
| 用户态白名单 `trs_sqcq.c:1482` 加 REG_BASE | "需加" | **不要加**——REG_BASE 真值在内核 `sq_ctx.reg_mem.uva`，必须走 ioctl；加进白名单反而走 UIO 读用户态表（无此字段）必失败 |
| 内核 query | "墙① has_res 拦截" | 能过（config 同检查实测通过）；唯一断点是 uva=0 |
| 内核 case | — | `case DRV_SQCQ_PROP_SQ_REG_BASE` 已存在（`:1668`），Patch 1 填好 uva 后自动通 |

## 四、验证链（补丁生效的判定）

```
1. 重编驱动 + 装载（宿主机）
2. 创建 CP 流（aclrtCreateStreamWithConfig(ACL_STREAM_DEVICE_USE_ONLY)）
3. CP1 内 halSqCqQuery(prop=5)  → 期望返回非 0 VA（现 65534）
4. kernel MMIO 直写: *(volatile u32*)(sqRegBase + 0x8) = newTail → SDMA 消费（v43 通道回归）
5. 设备日志侧: IsSqDbMap 由 0 变 1（闭源注册日志，辅助佐证）
```

## 五、风险与回退

- **唯一风险**：设备侧对 `TRS_MSG_SQ_REG_MAP` 消息的处理器（闭源固件/设备服务）
  是否支持 v2 芯片（dav-2002）。tsch 开源仓无此消息处理，无法静态确认。
- **失败表现**：`trs_host_msg_send` 返回非零 → 补丁函数返回错误 → `reg_mem.uva` 保持 0
  → 行为与现状完全一致（**不会更糟**）。
- **失败时的替代路线**：退回 §13 R3 位置 1（闭源 remote 注册 handler 补 db 页 mmap，
  走 `trs_sq_remap` 的页映射路线而非消息路线）。

---

# 六、交付物与操作指引（2026-10-15 实施完成）

## 交付物（三件套，全就绪）

| # | 物 | 位置 |
|---|---|---|
| 1 | **driver 仓改动分支** | `/home/w00930648/driver` 分支 `wgm-310p` @ commit `7b4467a`（60 行：2 函数迁移 + 1 include + ops 表 2 槽位） |
| 2 | **标准 patch 文件** | 本目录 `0001-feat-trs-stars_v2-sqcq_reg_map-unmap-remote-SQ-doorb.patch`（`git am` 可直接应用） |
| 3 | 源码坐标档案 | 寄存器文档 §17 + 本文档一~五章 |

改动内容 = 本文第二节 Patch 的机器执行版（逐字一致，含注释）。核对基线：开源仓 master
（gitcode.com/cann/driver）。

## 构建步骤（在有内核构建环境的机器上）

```bash
# 0. 前置: 目标机(wwl机器)内核版本的内源码/头 + aarch64 工具链
#    (wwl 机器: uname -r 取内核版本, /lib/modules/$(uname -r)/build 须存在)
cd <driver仓> && git checkout wgm-310p

# 1. 构建 (⚠ 见下方"构建目标警告")
bash build.sh --pkg --soc=ascend910b -j16 \
     -k /lib/modules/$(uname -r)/build

# 2. 产物: 输出目录中的驱动 run 包
```

### ⚠ 构建目标警告（重要）

开源仓 `build.sh` 只支持 `ascend910b / ascend910_93 / ascend950` 三个 `--soc`——
**没有 310P3 专属构建目标**（310P3 驱动包是华为内部构建线出的，如 24.1.rc3）。
两条路：

| 路 | 操作 | 风险 |
|---|---|---|
| A. 自编（试） | `--soc=ascend910b` 构建（trsdrv/stars_v2 源码在同一树，会被编入） | 安装包元数据（芯片校验/固件配套）可能不匹配 310P3 机器——安装脚本可能拒绝；强行装需绕过校验 |
| B. 提驱动团队（正规） | 把 `0001-*.patch` 交给驱动团队，在**内部 310P3 构建线**出版本 | 沟通周期 |

**建议先走 B**（patch 即提交单元，根因注释在 commit message 里写全了）；A 作为快验证手段。

## 装机与验证（装好驱动后）

```bash
# 1. 备份现驱动 (wwl 机器 24.1.rc3)
cp -r /usr/local/Ascend/driver /root/driver_bak_24.1.rc3

# 2. 安装自编/团队出的驱动包 (按包内说明; 注意版本号须 ≥24.1 以过 pypto 驱动门禁)

# 3. 重启设备驱动 (或整机重启), 确认 npu-smi info 正常

# 4. 验证链 (按序):
#   4.1 创建 CP 流: 任意 CUST/CP 流程序 (如 memfabric mode13 前置)
#   4.2 CP1 内查 prop=5:
#        期望: halSqCqQuery(SQ_REG_BASE) 返回 0 且 value 非 0   ← 补丁生效判据
#        (现值: 65534 NOT_SUPPORT; 测试载体: xcclpp cp1QueryProp / restore_probe)
#   4.3 MMIO 直写: *(volatile u32*)(sqRegBase + 0x8) = newTail → SDMA 消费 (v43 通道回归)
#   4.4 设备日志佐证: plog 搜 "IsSqDbMap=1" (原 IsHwSqMap=1 IsSqDbMap=0)

# 5. 失败判定与回退:
#   - 4.2 仍 65534 且 plog 出现 "Msg send fail" → 设备侧不支持 v2 的 SQ_REG_MAP 消息
#     → 补丁无害 (uva 保持 0 = 现状), 退回文档 §13 R3 位置 1 (闭源 handler 补 db 页 mmap)
#   - 驱动异常 → 回滚: 恢复 /root/driver_bak_24.1.rc3 + 重启
```

## 成功后的连锁收益（一改三收）

```
sq_ctx[sqid].reg_mem.uva 被填上
  → prop=5 查询通 (内核 case :1668 读 uva 返回)
  → hybm kernel 直写 doorbell: *(volatile u32*)(sqRegBase+0x8)=tail (ns 级, 替代逐笔 μs ioctl)
  → halSqCqConfig 的 UIO 直写路径激活 (sq_ctrl.db 非空, trs_sqcq.c:1156)
  → AICore 直驱 (shmem 三步流程) 在 310P3 解锁 — 小消息 80%+ 量级收益的门
```
