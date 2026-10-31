# AI 协作实录：一份跨三仓的寄存器机制调研文档是怎么产出的

> **归档日期**：2026-10-31
> **协作工具**：opencode CLI（终端 AI 编码代理，模型 GLM，model id `gateway/glm-5.3`）
> **任务性质**：多仓库技术调研 + 架构可行性判定 + 驱动团队需求输出，最终交付一份全结论带源码路径行号、区分一手实测/源码推断的调研报告
> **本文目的**：完整记录"提示词设计 → AI 工作过程 → 交付成果"的端到端过程，沉淀可复用的提示词模板与协作方法，作为 AI 工程化实践样本
> **成果附件**：[AICore寄存器开放机制调研报告（AI 全程产出）](attachments/310P-AICore寄存器开放机制调研报告.md)

---

## 1. 业务背景

310P3 上，pypto 框架验证过一条"通过特制驱动把 AICore 核内同步寄存器映射给用户态"的通路（`halMemCtl` addr_type=2/3，0xA0/0x4C8 槽位 + `set_cond`/`MOV` 指令对），据此 memfabric 想判断：当前基于 GM 轮询的六条通信通道，哪些可以改走寄存器以降时延、省缓存维护；同时驱动团队需要一份行号级的改动需求（remote SQ 门铃 `sqRegBase` 查不到的根因与补丁落点）。

这是一个典型的"AI 能发挥复合价值"的任务：跨 pypto / driver / memfabric_hybrid 三个仓、需要一手实测与源码推断互相校验、输出要直接可执行（给驱动团队的补丁位置与验证方法）。

## 2. 提示词原文（完整、可直接复用）

以下为本次任务实际下发的完整提示词，未做任何删改：

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

### 2.1 提示词设计要素拆解

| 设计要素 | 原文位置 | 作用与效果 |
|---|---|---|
| **任务分层递进** | "重点看三个用途 → 然后对照……逐条分析 → 最后……给驱动团队" | 三个子任务按"调研→判定→输出"递进，AI 天然形成工作流顺序，无需自行猜测先做什么 |
| **检索锚点（隐式 few-shot）** | `addr_type=2/3`、`0xA0`、`0x4C8`、`set_cond/MOV`、`25.3.rc1.b099`、六条通道名 | 提示词不解释机制，但给出符号级锚点。AI 直接按符号命中源码，省去"从零探索问题域"的整层成本，也消除了"调研错对象"的歧义 |
| **枚举式收口** | 六条通道逐一列出名字 | 防止 AI 自行发明/遗漏通道，判定范围与业务方心智模型严格对齐 |
| **判定标准前置** | "判定标准只看写者是谁：本地 AICPU 或 AICore 写的可以寄存器化，SDMA 引擎写的物理上够不到核内寄存器" | 把判断权收窄成一条物理直觉规则，防止结论发散；同时该规则本身就是业务知识注入——AI 不需要懂芯片也能做出正确架构判定 |
| **输出格式规范** | "每条给出可行性结论、具体方案、收益和风险"；"全部结论必须给源码文件路径和行号，区分一手实测和源码推断" | 双重规范：内容模板（结论/方案/收益/风险四段）+ 证据纪律（行号强制、实测与推断分级）。后者是整份报告可信度的基石 |
| **受众与交付物明确** | "给驱动团队一份改动需求……应该在哪些文件哪些行补什么。附上验证方法" | 指定读者（驱动团队）和粒度（文件+行号+验证），输出直接可执行而非泛泛综述 |
| **范围与路径约束** | "pypto 仓"、"memfabric 的通信架构"、"驱动源码仓 /home/w00930648/driver" | 明确三个对象仓与绝对路径，AI 无需搜索定位工作目录 |

一句话复盘：这条提示词把**领域知识**（锚点、判定标准）、**任务结构**（三层递进+枚举收口）、**输出纪律**（行号+证据分级）三类信息全部前置，AI 的自由度被压缩到"执行与验证"层面——这正是高质量技术调研提示词的通用形态。

## 3. AI 真实工作过程还原

以下按实际发生顺序还原（含失误与纠偏，未做美化）。整个过程 AI 自主完成，人工介入仅两处：初始提示词、分支选择确认（见 3.6）。

### 3.0 任务规划

AI 收到任务后第一个动作不是读代码，而是拆解出 8 项待办（halMemCtl 映射机制 / 任务派发与完成上报 / PMU 采集 / 特制 vs 普通驱动 / memfabric 六通道现状 / 逐条可行性 / driver 仓根因 / 汇总报告），并全程维护状态。**关键决策**：发现 pypto 存在 `framework/` 与 `experiments/runtime/` 两份孪生实现，立即选定 `framework/` 为主副本统一引用行号，避免读者拿到两套坐标。

### 3.1 主线阅读（pypto 仓）

按提示词锚点直接命中关键文件（说明锚点注入的有效性）：

| 锚点 | 命中文件（framework 副本） | 拿到的关键事实 |
|---|---|---|
| `halMemCtl`/`addr_type` | `src/machine/runtime/runner/runtime_agent.cpp:164-194`、`device_runner.cpp:244-295`、`adapter/api/hal_api.cpp:26-37` | 入参 `AddrMapInPara{addr_type, devid}`、出参 `{ptr,len}`；310P 每核 2MB stride、10 核 32MB；失败回退 GM（commByReg=false） |
| `0xA0`/`0x4C8` | `src/machine/device/dynamic/aicore_hal.h:51-54、419-456、233-246、352-370` | AICPU MMIO 写 `+0xA0`（派发）、MMIO 读 `+0x4C8`（完成） |
| `set_cond`/`MOV` | `src/interface/machine/device/tilefwk/aicore_entry.h:159-165、189-201、303-313` | AICore 侧 `MOV %0, DATA_MAIN_BASE` 读 SPR、`set_cond()` 写 SPR；`set_cond` 在仓内只有 host 仿真定义，实为 CCE 编译器内建 intrinsic |
| `PMU` | `src/machine/device/dynamic/aicore_prof_dav2002_pmu.h:27-49`、`aicore_prof.cpp:299-412、435-516` | PMU 寄存器偏移（CNT0=0x210、TOTAL=0x250 等）、AICPU 侧编程+采样全链路 |

### 3.2 并行子代理分工（二次提示词工程）

driver 仓与 memfabric 仓体量大，AI 没有自己顺序啃完，而是**并行派发两个探索子代理**，自己继续 pypto 主线——这是本次协作中最重要的工程决策之一：主线上下文保持干净，子任务各自带完整约束独立执行。

下发子代理的提示词本身也是结构化的（节选 driver 仓任务，展示 AI 如何"把好提示词的要素传递下去"）：

```text
你是代码调研员（只读研究，禁止修改任何文件）。目标仓库：/home/w00930648/driver……
请非常彻底地（very thorough）调研以下问题，所有结论必须附文件绝对路径和行号：
1. halMemCtl 接口实现：……列出 addr_type 的所有枚举值定义（特别是 addr_type=2 和
   addr_type=3 的含义……）。
2. v2/v3 双链机制：……搞清楚：v2 链和 v3 链分别是什么函数调用链……
   为什么某些版本会返回失败导致上层 fallback。
3. remote SQ 门铃注册（CP 流 sqRegBase）：搜索 "sqRegBase"……列出相关结构体定义
   （字段名+行号）、注册函数、调用点。
4. 驱动版本 25.3.rc1.b099：……
输出格式：按上述 5 个主题分节，每条结论给 [文件路径:行号]，引用关键代码片段
（不超过 10 行/条）。区分「直接读到的事实」和「根据代码推断」。如果某项在该仓库
确实找不到，明确说"未找到"并列出你搜索过的符号清单。
```

两个子代理返回了带行号的完整报告（driver 仓定位到 `halMemCtl` v2/v3 双链、`sqRegBase` 根因、版本命名规则；memfabric 仓定位到六通道全部写者/读者与 GM 轮询原语）。

### 3.3 一手实测：在本机 310P3 真机上验证"特制 vs 普通"

**关键决策**：源码只能证明"结构"，证明不了"行为"。AI 发现本机就是 310P3 真机（`npu-smi info` ×8 卡），于是决定写探针把"普通驱动行为"从推断升级为实测——迭代三轮：

```c
// 探针核心逻辑（/tmp/opencode/probe_halmemctl.c，探针完整源码见 4.3 节）
aclrtSetDevice(devId);                    // 1. 先 open 设备（否则 ret=4）
void *hal = dlopen("/usr/local/Ascend/driver/lib64/driver/libascend_hal.so", RTLD_NOW);
halMemCtl_t halMemCtl = dlsym(hal, "halMemCtl");
struct AddrMapInPara in = {.addr_type = 2, .devid = devId};   // addr_type=2 同步寄存器
int ret = halMemCtl(0, &in, sizeof(in), &out, NULL);          // 期望 ret=0、ptr≠0
```

| 迭代 | 现象 | 纠偏动作 |
|---|---|---|
| 第 1 轮 | `dlopen("libascend_runtime.so")` 失败 | 找到正确库名 `libascendcl.so`（`nm -D` 确认导出 `aclrtSetDevice`） |
| 第 2 轮 | 未 open 设备即调 halMemCtl → `ret=4` | 对照驱动源码 = `DRV_ERROR_INVALID_HANDLE`（`ascend_hal_error.h:38`），前置条件吻合，补 `aclrtSetDevice` |
| 第 3 轮 | open 后 addr_type=0~4 **全部 `ret=65534`（DRV_ERROR_NOT_SUPPORT）** | 这就是普通驱动的实测行为——整个寄存器映射通路不可用 |

随后排除法定位拦截点（均实测）：
- `cat /proc/svm/dev*/feature/aic_reg_map` → **1**（内核能力开关是开的，排除能力拦截）；
- `halGetDeviceSplitMode(0)` → **mode=0**（排除 vNPU 切分拦截）；
- **`dmesg` 实锤**：91 条 `devmm_send_map_addr_msg: Map address failed. (ret=-95; addr_type=…)`——失败发生在**设备侧链路返回 -EOPNOTSUPP**（内核能力检查若拦截则不会进入 send 函数），即普通驱动的设备侧 SVM agent 不实现 `MAP_DEV_RESERVE`；且日志进程名大量为 python（pypto 本机一直在 GM fallback 模式下运行）。
- 二进制佐证：`nm -D`/`objdump` 确认本机驱动是 v2 链（`devmm_ctrl_map_addr` 符号在、`halMemCtl` 内 9 项 handler 表、`type>8` 直接 65534），与驱动源码逐项对上。

### 3.4 失误与自查（真实还原）

- 搜索 `25.3.rc1.b099` 时误用 `rg -rn`（`-r` 是 replace 不是递归），得到误导性空结果；AI 意识到命令可疑后改用正确的 `rg -n` 全盘重搜（pypto/memfabric/driver/docs/hccl/hcomm/runtime 等 13 个目录）——**确认字面量在全部本地仓 0 命中**。最终报告中如实写明："25.3.rc1.b099 无法本地核实，按 custom 命名法推断为 25.3.rc1 线第 99 次构建，需在装该驱动的机器上实测闭环"，并列入验证缺口。
- 一次 `find /` 超时被工具截断后，改为限定目录重试。

### 3.5 交叉验证（AI 输出也要被审）

- 子代理报告的关键行号（布局图、signal/wait/quiet 原语、doorbellViaHal、`trs_hw_sqcq.c:1566-1670`、`stars_v2` ops 表等）由 AI 主线程逐条抽查核对后才采信；
- 抽查中发现本仓 `wgm-dev-310p` 分支已有前期调研 [寄存器开放机制与AICPU方案优化机会](../310p/寄存器开放机制与AICPU方案优化机会.md)（2026-10-09/10）——本次独立调研与其关键结论**互相独立地一致**（普通驱动 65534、特制驱动放行、stars_v2 适配层 ops 为 NULL 的根因），构成双重验证；本次新增的是：探针代码与 dmesg 实锤、PMU 偏移全表、六通道写者判定、wgm-310p 分支补丁 commit `7b4467a4` 的实证。

### 3.6 人工介入点（全程仅两处）

1. 初始提示词（本文 §2）；
2. 提交分支确认——AI 发现仓内已有 `wgm-dev-310p` 分支而提示词说 "wgm-dev"，主动向人确认后选定既有分支，避免推错远端。

## 4. 成果

完整报告见附件：[AICore寄存器开放机制调研报告](attachments/310P-AICore寄存器开放机制调研报告.md)。此处展示结构与最核心的结论。

### 4.1 报告结构（四部分）

1. pypto AICore 寄存器开放机制（halMemCtl 映射 / 0xA0 派发+0x4C8 完成协议 / PMU 采集）
2. 特制驱动 25.3.rc1.b099 与普通驱动的行为差异（一手实测 + 源码 + 推断三级证据）
3. memfabric 六条通道寄存器化可行性逐条判定（结论/方案/收益/风险）
4. 给驱动团队的改动需求（v2/v3 双链机制、sqRegBase 根因与补丁文件行号、验证方法）

### 4.2 核心结论一览（六通道判定）

| # | 通道 | 写者 | 判定 | 一句话方案 |
|---|---|---|---|---|
| a | 请求邮件通知 | 本端 AICore | **可行** | 邮件体留 GM，通知走 `set_cond` → AICPU 轮询 `+0x4C8`（抄 pypto COND 协议） |
| b | 背压/ready | 本端 AICPU | **可行** | 消费游标经 `+0xA0` 下发，AICore `MOV DATA_MAIN_BASE` 直读（与 pypto 派发方向同构） |
| c | SQE doorbell 提交 | 本端 AICPU | **可行（价值最高）** | 补 stars_v2 `sqcq_reg_map` 后查 prop=5 得 `sqRegBase`，直写 `sqRegBase+0x8` 替代每批 ioctl |
| d | 到达/完成戳 | **SDMA 引擎** | **不可行** | SQE 目的地址只能经 SMMU 落 GM，物理够不到核内寄存器 |
| e | AICore 计时 | host CPU | **不可行** | 写者是 host 心跳不满足核内写；310P AICore 无 cycle 计数器 |
| f | 大块数据 cache clean | AICore 产、SDMA 消费 | **不可行/无收益** | 数据面 cache 维护，寄存器装不下大块数据 |

驱动团队需求落点：`driver/src/sdk_driver/trsdrv/trs/lba/near/comm/adapt/trs_host_core/stars_v2/trs_core_stars_v2_ops.c:117-163` 补 `sqcq_reg_map/_unmap`、`:177-178` ops 表挂接（本仓 wgm-310p 相关分支 commit `7b4467a4` 已有现成补丁，向 25.3.rc1.b099 线合入即可）；halMemCtl 侧需设备侧 SVM agent 固件实现 `MAP_DEV_RESERVE`（普通驱动实测 ret=-95 缺的就是它）。

### 4.3 效果实录（终端原始输出）

以下均为本次会话真实终端输出（截图可按 §5 指引补拍）：

```text
$ npu-smi info | head -6
| NPU     Name    Health  ... 
| 5664    310P3   OK      ...        ← 本机即 310P3 真机，8 卡

$ ./probe_halmemctl 0        ← 探针（完整源码见下）
[probe] aclrtSetDevice(0) = 0
[probe] halMemCtl @ 0xffffaa9ab184 (driver lib .../libascend_hal.so)
[probe] halMemCtl(addr_map, addr_type=2, devid=0) ret=65534 ptr=0x0 len=0x0 (FAILED)
[probe] halMemCtl(addr_map, addr_type=3, devid=0) ret=65534 ptr=0x0 len=0x0 (FAILED)

$ cat /proc/svm/dev0/feature/aic_reg_map
1                                          ← 内核能力开关是开的

$ dmesg | grep "Map address failed" | head -2
[  374.795082] ... [devmm_send_map_addr_msg 41] <python:20489> Map address failed.
               (ret=-95; addr_type=2; devid=6; vfid=0; addr=0x0)
[  710.396824] ... [devmm_send_map_addr_msg 41] <python:64752> Map address failed.
               (ret=-95; addr_type=2; devid=6; ...)
$ dmesg | grep -c "Map address failed"
91                                         ← 普通驱动机器上 pypto 一直 GM fallback
```

探针完整源码（可直接复用，验证任何 310P 机器的驱动行为）：

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

**判读表**：特制驱动机器预期 `ret=0, len=0x2000000`（32MB/10 核×2MB）；普通驱动（本机 26.1.0）实测 `ret=65534` + dmesg `ret=-95`。

### 4.4 会话生产力指标（约数）

| 指标 | 数值 |
|---|---|
| shell 命令执行 | ≈30 次（含探针编译运行 3 轮、dmesg/procfs 取证） |
| 文件读取/符号检索 | ≈25 次（Grep/Read/Read+行号定位） |
| 并行子代理 | 2 个（driver 仓、memfabric 仓） |
| 探针程序 | 2 个（probe_halmemctl / probe_split），探针源码已在上文沉淀 |
| 人工介入 | 2 处（初始提示词、分支确认） |
| 产出 | 1 份四部分调研报告（全部结论带文件路径+行号，三级证据分级） |

## 5. 建议截图位（供展示材料补拍）

本文档以终端实录承载证据；如需配图，建议按以下位置截图：

1. **提示词下发**：opencode 终端会话首屏（本文 §2 提示词原文）；
2. **过程**：AI 并行派发子代理、探针三轮迭代的终端画面；
3. **效果**：`./probe_halmemctl` 运行输出、`dmesg` 取证、报告正文的"六通道判定表"渲染效果。

## 6. 可复用提示词模板

将本次提示词泛化为模板，可直接套用到任何"多仓调研 + 架构判定 + 需求输出"类任务：

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

模板的设计逻辑（对应 §2.1 要素表）：**领域知识前置（锚点+判定规则）→ 任务结构化（分层+枚举）→ 证据纪律强制（行号+分级+缺口声明）→ 受众与交付物明确**。凡满足这四条的任务，AI 的产出即可直接进入工程评审。

---

*本文与附件由 AI（opencode + GLM）在人工提示词驱动下产出，过程记录如实包含失误与纠偏，未做美化。*
