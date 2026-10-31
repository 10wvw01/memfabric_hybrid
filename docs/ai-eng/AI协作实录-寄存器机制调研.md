# 我用 AI 协作产出《AICore 寄存器开放机制调研报告》的记录

> **日期**：2026-10-31
> **工具**：opencode（终端 AI 编码代理，GLM 模型）
> **成果**：[AICore寄存器开放机制调研报告](attachments/310P-AICore寄存器开放机制调研报告.md)（全结论带源码路径行号，区分一手实测/源码推断）
> **本文记录**：从提示词设计、过程把控到成果复核的完整协作过程，供团队参考复用

---

## 1. 起因

10 月底我手上有一份调研要做，三个诉求叠在一起：

1. **机制调研**——pypto 仓在 310P 上跑通过"AICore 核内寄存器映射给用户态"这条路（`halMemCtl`、0xA0/0x4C8、`set_cond`/`MOV`），要把机制吃透；
2. **架构判定**——memfabric 现行六条 GM 轮询通道，哪些能换成寄存器；
3. **驱动需求**——remote SQ 门铃 `sqRegBase` 查不到的根因和补丁落点，要精确到文件行号，给驱动团队直接用。

难点不在写文档，在**把三个仓的源码读对，有真机的部分实测到**。手工做要通读 pypto / driver / memfabric 三仓的关键路径再上板跑探针。我决定用 opencode 协作：**我出提示词、做方向决策、做结论把关；它出调研执行和验证。**

## 2. 我给它的提示词

原文如下，一个字没改：

```text
调研 pypto 仓的 AICore 寄存器开放机制。重点看三个用途：halMemCtl 怎么映射
寄存器区域（addr_type=2 同步寄存器、addr_type=3 PMU）、AICPU 和 AICore 之间
怎么通过寄存器做任务派发和完成上报（0xA0 ready 队列、0x4C8 finish 队列、
set_cond/MOV 指令）、PMU 性能计数器怎么采集。同时查清特制驱动 25.3.rc1.b099
和普通驱动的行为差异。

然后对照我们 memfabric 的通信架构，逐条分析六条通道（请求邮件通知、背压/ready、
SQE doorbell 提交、到达/完成戳、AICore 计时、大块数据 cache clean），
判断每条能否用寄存器替代现有的 GM 轮询方案。判定标准只看写者是谁：
本地 AICPU 或 AICore 写的可以寄存器化，SDMA 引擎写的物理上够不到核内寄存器。
每条给出可行性结论、具体方案、收益和风险。

最后，基于驱动源码仓 /home/w00930648/driver，给驱动团队一份改动需求：
halMemCtl 的 v2/v3 双链机制是什么、CP 流 remote SQ 的门铃注册为什么查不到
sqRegBase、应该在哪些文件哪些行补什么。附上验证方法。

全部结论必须给源码文件路径和行号，区分一手实测和源码推断。
```

### 2.1 我为什么这么写

| 设计点 | 我的考虑 |
|---|---|
| **开头不解释背景，直接给锚点**（addr_type=2/3、0xA0、0x4C8、set_cond/MOV、25.3.rc1.b099）| 这些符号我已经知道存在。给了锚点，AI 就不需要"先搞清楚问题域再找代码"，直接按符号命中源码，也杜绝调研错对象 |
| **六条通道逐一列名** | 判定范围必须和我的心智模型严格对齐，不给它自由发挥"通道分类学"的空间 |
| **判定标准写成一句物理规则**（"只看写者是谁……SDMA 引擎写的物理上够不到核内寄存器"）| 这条规则是这次判定的全部依据。规则前置，AI 只需要执行判定，不需要替我做架构决策——决策错了算我的，不能算它的 |
| **强制行号 + 实测/推断分级** | 报告要进评审、要给别人复核。没有行号的结论没法复核，混着推断的"实测"会误导决策，所以这条放在最后单独一句，权重拉满 |
| **指定受众和交付粒度**（"给驱动团队""哪些文件哪些行补什么""附上验证方法"）| 受众决定文风和粒度。给驱动团队的东西，综述没有意义，必须能直接开工 |

一句话：**领域知识前置（锚点+规则）、任务结构化（分层+枚举）、证据纪律强制（行号+分级）**。事后看，这三条是这次提示词有效的全部原因。

## 3. 过程记录

以下按实际发生顺序记录。整个过程我的人工介入只有两处（§3.6），其余由 AI 自主执行、我在旁边看。

### 3.1 下发提示词后

它的第一个动作不是读代码，而是把任务拆成 8 项待办（映射机制 / 派发与完成 / PMU / 特制 vs 普通驱动 / 六通道现状 / 逐条判定 / driver 根因 / 汇总）挂在任务列表上。然后做对了一个细节：pypto 仓有 `framework/` 和 `experiments/runtime/` 两份孪生实现，它先选定 `framework/` 为行号引用基准——两份代码行号有漂移，不先定基准，报告里的坐标会打架。

### 3.2 主线阅读

按我给的锚点直接命中关键文件：

| 锚点 | 命中位置（pypto framework 副本） | 拿到的事实 |
|---|---|---|
| `halMemCtl`/`addr_type` | `src/machine/runtime/runner/runtime_agent.cpp:164-194`、`device_runner.cpp:244-295` | 入参 `{addr_type, devid}` 出参 `{ptr,len}`；310P 每核 2MB、10 核共 32MB；失败回退 GM（commByReg=false） |
| `0xA0`/`0x4C8` | `src/machine/device/dynamic/aicore_hal.h:51-54、419-456` | AICPU MMIO 写 `+0xA0`（派发）、MMIO 读 `+0x4C8`（完成） |
| `set_cond`/`MOV` | `src/interface/machine/device/tilefwk/aicore_entry.h:159-165、189-201` | AICore 侧 `MOV %0, DATA_MAIN_BASE` 读 SPR、`set_cond()` 写 SPR；`set_cond` 是 CCE 编译器内建 intrinsic |
| `PMU` | `src/machine/device/dynamic/aicore_prof_dav2002_pmu.h:27-49` 等 | PMU 寄存器偏移全表、AICPU 侧编程+采样链路 |

### 3.3 它自己拆了并行子代理

driver 仓和 memfabric 仓体量大，它没有自己顺序啃，而是派了两个并行子代理分头调研，主线继续啃 pypto。给它子代理的任务书也是结构化的——输出格式（按主题分节、每条结论带 `[文件路径:行号]`）、事实与推断分离、**找不到就明说"未找到"并列出搜索过的符号**。这个约束后来起了作用（见 §3.5）。

### 3.4 本机实测：超出我预期的部分

源码只能证明"结构"，证明不了"行为"。它注意到**本机就是 310P3 真机**（`npu-smi` ×8 卡），于是写探针把"普通驱动行为"从推断升级成实测——探针迭代了三轮：

```c
// 探针核心逻辑（完整源码见 §4.3）
aclrtSetDevice(devId);                    // 1. 先 open 设备（否则 ret=4）
void *hal = dlopen(".../driver/lib64/driver/libascend_hal.so", RTLD_NOW);
halMemCtl_t halMemCtl = dlsym(hal, "halMemCtl");
struct AddrMapInPara in = {.addr_type = 2, .devid = devId};
int ret = halMemCtl(0, &in, sizeof(in), &out, NULL);   // 期望 ret=0
```

| 迭代 | 现象 | 纠偏 |
|---|---|---|
| 1 | dlopen `libascend_runtime.so` 失败 | 找到正确库名 `libascendcl.so` |
| 2 | 未 open 设备 → `ret=4` | 对照驱动源码 = `DRV_ERROR_INVALID_HANDLE`，前置条件吻合，补 `aclrtSetDevice` |
| 3 | open 后 addr_type=0~4 **全部 65534（NOT_SUPPORT）** | 这就是普通驱动的实测行为 |

然后排除法定位拦截点：`/proc/svm/dev*/feature/aic_reg_map` = 1（能力开着）、`halGetDeviceSplitMode` = 0（非 vNPU 切分）→ 都不是。最后 `dmesg` 出实锤：**91 条 `Map address failed. (ret=-95)`，失败点在设备侧链路返回 EOPNOTSUPP**——普通驱动缺的是设备侧 SVM agent 固件对 `MAP_DEV_RESERVE` 的实现。日志里失败进程名大量是 python，即本机 pypto 一直在 GM fallback 模式下跑。这一段把"特制 vs 普通"的差异从猜测变成了有 dmesg 背书的结论。

### 3.5 翻车与纠偏（真实发生过，照记）

它搜 `25.3.rc1.b099` 时先用了 `rg -rn`（`-r` 是 replace 不是递归），拿到误导性空结果；后来自己起疑，用正确命令把 13 个目录全盘重搜，**确认该字面量在所有本地仓 0 命中**。最终报告没有硬圆，而是如实写进验证缺口："25.3.rc1.b099 无法本地核实，按 custom 命名法推断为 25.3.rc1 线第 99 次构建，需在装该驱动的机器上实测闭环"。我认可这个处理——查不到就说查不到，比编一个结论强。

### 3.6 我的两处介入

1. **初始提示词**（§2）；
2. **提交分支**——本文档入仓时，它发现提示词里说的 `wgm-dev` 分支不存在、仓里实际是 `wgm-dev-310p`，停下来问我而不是自作主张，我拍板用后者。

### 3.7 我为什么敢直接采用它的结论

三个原因，都写进了报告本身：

- **全量行号**：任何一条结论都能打开文件复核。它对子代理返回的行号也做了逐条抽查后才采信（布局图、doorbell 原语、`trs_hw_sqcq.c:1566-1670`、stars_v2 ops 表等，我抽查过几处，命中）；
- **证据分级**：【实测】/【源码】/【推断】三级标注贯穿全文，推断明确标注、缺口明确声明；
- **交叉一致**：仓里 `docs/310p/` 有一份 10 月上旬的[前期调研](../310p/寄存器开放机制与AICPU方案优化机会.md)，这次是独立重做，关键结论（普通驱动 65534、特制放行、stars_v2 适配层 ops 为 NULL 的根因）两条路径殊途同归，互相印证。本次新增的价值在：探针+dmesg 实锤、PMU 偏移全表、六通道写者判定、wgm-310p 分支补丁 `7b4467a4` 的实证。

## 4. 成果

完整报告见附件（四部分：机制调研 / 特制 vs 普通驱动 / 六通道判定 / 驱动改动需求）。核心结论：

| # | 通道 | 写者 | 判定 | 一句话方案 |
|---|---|---|---|---|
| a | 请求邮件通知 | 本端 AICore | **可行** | 邮件体留 GM，通知走 `set_cond` → AICPU 轮询 `+0x4C8`（抄 pypto COND 协议） |
| b | 背压/ready | 本端 AICPU | **可行** | 消费游标经 `+0xA0` 下发，AICore `MOV DATA_MAIN_BASE` 直读 |
| c | SQE doorbell 提交 | 本端 AICPU | **可行（价值最高）** | 补 stars_v2 `sqcq_reg_map` 后查 prop=5 得 `sqRegBase`，直写 `sqRegBase+0x8` 替代每批 ioctl |
| d | 到达/完成戳 | **SDMA 引擎** | **不可行** | SQE 目的地址只能经 SMMU 落 GM，物理够不到核内寄存器 |
| e | AICore 计时 | host CPU | **不可行** | 写者是 host 心跳不满足核内写；310P AICore 无 cycle 计数器 |
| f | 大块数据 cache clean | AICore 产、SDMA 消费 | **不可行/无收益** | 数据面 cache 维护，寄存器装不下大块数据 |

驱动需求落点：`driver/src/sdk_driver/trsdrv/trs/lba/near/comm/adapt/trs_host_core/stars_v2/trs_core_stars_v2_ops.c:117-163` 补 `sqcq_reg_map/_unmap`、`:177-178` ops 表挂接（wgm-310p 分支 commit `7b4467a4` 已有现成补丁，向 25.3.rc1.b099 线合入）；halMemCtl 侧需设备侧 SVM agent 固件实现 `MAP_DEV_RESERVE`（普通驱动实测 ret=-95 缺的就是它）。

### 4.3 效果实录（终端原始输出，截图可按 §5 指引补拍）

```text
$ npu-smi info | head -6
| NPU     Name    Health  ...
| 5664    310P3   OK      ...        ← 本机即 310P3 真机，8 卡

$ ./probe_halmemctl 0        ← 探针
[probe] aclrtSetDevice(0) = 0
[probe] halMemCtl @ 0xffffaa9ab184 (driver lib .../libascend_hal.so)
[probe] halMemCtl(addr_map, addr_type=2, devid=0) ret=65534 ptr=0x0 len=0x0 (FAILED)
[probe] halMemCtl(addr_map, addr_type=3, devid=0) ret=65534 ptr=0x0 len=0x0 (FAILED)

$ cat /proc/svm/dev0/feature/aic_reg_map
1                                          ← 内核能力开关是开的

$ dmesg | grep -c "Map address failed"
91                                         ← 普通驱动机器上 pypto 一直 GM fallback
```

探针完整源码（可直接复用，判读：特制驱动机器预期 `ret=0, len=0x2000000`；普通驱动 `ret=65534` + dmesg `ret=-95`）：

```c
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
struct AddrMapInPara { unsigned int addr_type; unsigned int devid; };
struct AddrMapOutPara { unsigned long long ptr; unsigned long long len; };
typedef int (*halMemCtl_t)(int type, void *param, size_t size, void *out, size_t *outRet);
typedef int (*aclrtSetDevice_t)(int32_t);
int main(int argc, char **argv) {
    int devId = (argc > 1) ? atoi(argv[1]) : 0;
    void *rt = dlopen("/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64/libascendcl.so", RTLD_NOW);
    aclrtSetDevice_t setDev = (aclrtSetDevice_t)dlsym(rt, "aclrtSetDevice");
    printf("[probe] aclrtSetDevice(%d) = %d\n", devId, setDev(devId));
    void *hal = dlopen("/usr/local/Ascend/driver/lib64/driver/libascend_hal.so", RTLD_NOW);
    halMemCtl_t halMemCtl = (halMemCtl_t)dlsym(hal, "halMemCtl");
    for (unsigned int addrType = 2; addrType <= 3; addrType++) {
        struct AddrMapInPara in = {.addr_type = addrType, .devid = (unsigned int)devId};
        struct AddrMapOutPara out = {.ptr = 0, .len = 0};
        int ret = halMemCtl(0 /*CTRL_TYPE_ADDR_MAP*/, &in, sizeof(in), &out, NULL);
        printf("[probe] halMemCtl(addr_map, addr_type=%u, devid=%d) ret=%d ptr=0x%llx len=0x%llx (%s)\n",
               addrType, devId, ret, out.ptr, out.len, ret == 0 ? "OK" : "FAILED");
    }
    return 0;
}
```

### 4.4 过程成本（约数）

| 指标 | 数值 |
|---|---|
| AI 侧：shell 命令 / 文件与符号检索 / 并行子代理 | ≈30 次 / ≈25 次 / 2 个 |
| 探针程序 | 2 个，3 轮迭代 |
| 我的人工介入 | 2 处（提示词、分支决策） |

## 5. 建议截图位

1. **提示词下发**：opencode 终端会话首屏（§2 提示词原文）；
2. **过程**：并行子代理、探针迭代的终端画面；
3. **效果**：`./probe_halmemctl` 输出、`dmesg` 取证、六通道判定表渲染效果。

## 6. 模板：这次提示词的泛化版

```text
【角色与目标】
调研 <仓库列表/绝对路径>。目标：<一句话业务目标>。

【任务分层】
1. 机制调研：重点看 <n> 个用途：<用途1>、<用途2>、<用途3>。
   （每个用途带上已知的符号/偏移/版本号锚点，如 <0xA0 / addr_type=2 / XXX.b099>）
2. 对照分析：对照 <自有系统> 的 <n> 条通道/模块（逐一枚举：<a>、<b>、<c>…），
   判定每条能否 <改造方向>。
   判定标准只看 <一条可执行的物理/业务规则>：<规则原文>。
   每条给出：可行性结论、具体方案、收益、风险。
3. 需求输出：面向 <受众团队>，给出 <改动/补丁/规格> 需求：
   <根因问题> 是什么、为什么 <现象>、应该在哪些文件哪些行补什么。附上验证方法。

【输出格式规范】
- 全部结论必须给源码文件路径和行号；
- 区分一手实测 / 源码推断（/推断），无法核实的明确写入验证缺口；
- 有真机环境时优先实测，探针代码随报告沉淀。
```

---

**后记**：本文档本身也是这次协作的一部分——第一版是 AI 以自己为主语写的"工作自述"，我看完否掉了：记录的主体应该是我怎么用它，不是它怎么工作。让它换回我的视角重写，才有了现在这版。整个过程我投入最多的是提示词和两处决策，中间没有逐行喂过代码。
