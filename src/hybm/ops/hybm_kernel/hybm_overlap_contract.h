// hybm_overlap_contract.h — MemFabric 310P AICore×AICPU 协同 kernel 的 wire 契约
// (host ↔ AICPU 编排 kernel 共享, 字段含 64 位设备地址, 错位 = 非法地址)
//
// 单一事实源: kernel (hybm_overlap_kernel.cc, hcc) 与 host 侧 (hybm_overlap_prep.cpp /
// hybm_overlap_capi.cpp) 一律 include 本头 — static_assert 编译期锁布局, 任一副本漂移即报错。
//
// 溯源: fork 自 xcclpp main examples/tutorials/aicpu-aicore-310p3/overlap_wire.hpp (MIT),
// 按 310p-aicore-aicpu-开发进展.md P0-E0a 迁移; 重命名 XcclppAicpuSqe* → HybmOverlap*。
// v2 精简: 仅保留流水编排 —— mode13 闭环由 chunks=1 等价覆盖, role1 编排方与全部
// 实验位(role/roleRsv/arrivalAddr/readyAddr/reserved)移除; 布局变化, 设备侧版本名
// 随 MF_OVERLAP_KVER bump(CP1 按文件名 dlopen 缓存纪律)。
//
// 约束: 纯 POD (无 STL/无构造函数), 须同时可被 hcc / g++ / bisheng 三种工具链编译。
//
// SPDX-License-Identifier: MIT

#pragma once               // 包含守卫(三工具链均支持的非标准指令)
#include <cstdint>        // 定宽整型(uint32_t/int32_t/uint64_t 等)
#include <cstddef>        // offsetof 宏(布局锁 static_assert 依赖)

// ============================================================================
// 提交模式 (param.submitMode)
// ============================================================================
enum SubmitMode : uint32_t { // 提交模式枚举(底层类型显式 u32, wire 尺寸锁定)
    SUBMIT_MODE_PIPELINE = 14, // 多 chunk 流水编排 (chunks = chunk 数; chunks=1 即单块语义)
    // 支持域外的值 kernel 拒绝 (mainRet=950)
};

// ============================================================================
// kernel 入参 (host 经 aclrtLaunchKernelWithHostArgs 传入)
// ============================================================================
struct HybmOverlapParam {        // kernel 入参 wire 结构(120B, 纯 POD)
    uint32_t devId;            // CP1 内 HAL devId (die 号 = host 设备号 % 2)
    uint32_t sqId;             // CP 流 SQ id (活查询用)
    int32_t streamId;          // CP 流 id (SQE 的 rt_streamid)
    uint32_t submitMode;       // SubmitMode
    uint64_t dstAddr;          // 数据搬运目的基址 (源地址经邮箱描述符传入, kernel 内逐 chunk 递增)
    uint64_t overflowAddr;     // SQE overflow 目标地址 (工作区 64B 槽)
    uint64_t flagSentinelAddr; // 哨兵源地址 (8B, SQE 对第二支的搬运源)
    uint64_t flagDoneAddr;     // 标志数组目的基址 (kernel 内逐 chunk +8)
    uint64_t resultAddr;       // kernel 结果写回地址 (工作区 56B 槽)
    uint32_t chunks;           // chunk 数 (1~64, 受邮箱槽数约束; 0 视为 1)
    uint32_t reserved[1];      // 对齐保留(补齐 8B, 无功能位)
    uint64_t mailboxAddr;      // AICore→AICPU 邮箱 ring 基址 (16B/slot {dataAddr, dataWords}, AICore 写)
    uint64_t timelineAddr;     // 打点: 本执行体 timeline 区 (16×u64, 0=关闭)
    uint32_t reserved2[10];    // 尾部保留 (补齐 120B, 未来扩展, 现无功能位)
};

// 结果结构 (AICPU 写, host 读)
struct HybmOverlapResult {     // 结果 wire 结构(56B, 诊断面包屑)
    uint32_t magic;        // 0xA1C0DE01: kernel 真正执行过
    uint32_t stage;        // 1=entry 5=done(v2 仅两态, 3/4 为历史中间态值已废弃)
    int32_t mainRet;       // 主返回码(0=成功, 13xx 段定位)
    int32_t subRet;         // 子返回码(次要错误补充)
    uint32_t sqBaseLow;     // SQ 环基址低 32 位(活查询证据)
    uint32_t sqDepth;       // SQ 环深度(槽数)
    uint32_t sqTail;        // 提交后 SQ tail(即 headBefore)
    uint32_t sqeCount;      // 活查询五属性返回码 nibble 打包/面包屑
    uint32_t tailAfter;     // 结束时 tail(诊断)
    uint32_t headAfter;    // head 前进 = TSFW 消费了
    uint64_t sqRegBase;
    uint32_t mapsBytes;
    uint32_t pad;          // 主链: 高16位=headBefore 低16位=ssid
};

constexpr uint32_t HYBM_OVERLAP_RESULT_MAGIC = 0xA1C0DE01; // 结果魔数(与 magic 字段同值)

enum Stage { // 状态机阶段枚举(结果 stage 字段取值)
    STAGE_ENTRY = 1, // 入口(kernel 已开始执行)
    STAGE_DONE = 5,  // 完成(编排完毕, head 前进已确认); 3/4 历史中间态已废弃, 值域不复用
};

// 通道元数据 (shmem stars_channel_info_t wire 契约, AICPU 查询算子填写)
struct StarsChannelInfo {      // 通道元数据结构(64B, CP1 活查询结果)
    uint32_t sqHead;        // SQ head(TSFW 消费位置)
    uint32_t sqTail;        // SQ tail(提交位置)
    uint64_t sqBase;        // SQ 环设备基址(SQE 直写目标)
    uint64_t sqRegBase;     // SQ 寄存器基址(v2 不再查询, 恒 0)
    uint32_t sqDepth;       // SQ 环深度
    uint32_t sqId;          // SQ id
    uint32_t cqId;          // CQ id(完成队列)
    uint32_t logicCqId;     // 逻辑 CQ id
    uint64_t cqeAddr;       // CQE 地址(完成队列项)
    uint32_t reportCqeNum;  // 上报 CQE 数
    uint32_t streamId;      // 所属流 id
    uint32_t devId;         // 所属设备 id
    uint8_t reserved[4];    // 对齐保留(补齐 64B)
};

// 布局锁死 — 任一副本漂移 (字段增删/类型变化) 在编译期报错
static_assert(sizeof(HybmOverlapParam) == 120, "HybmOverlapParam 布局漂移"); // 入参总尺寸锁 120B
static_assert(sizeof(HybmOverlapResult) == 56, "HybmOverlapResult 布局漂移"); // 结果总尺寸锁 56B
static_assert(sizeof(StarsChannelInfo) == 64, "StarsChannelInfo 布局漂移"); // 通道元数据尺寸锁 64B
static_assert(offsetof(HybmOverlapParam, chunks) == 56, "Param chunks 偏移漂移"); // chunks 字段偏移锁 56
static_assert(offsetof(HybmOverlapParam, mailboxAddr) == 64, "Param mailbox 偏移漂移"); // 邮箱字段偏移锁 64
static_assert(offsetof(HybmOverlapResult, sqRegBase) == 40, "Result sqRegBase 偏移漂移"); // 寄存器基址偏移锁 40
