// smem_mm_iar310p3 on dav-m200（Ascend 310P3）: TP2 MatMul + AllReduce 通算融合算子
//（硬件归约版。研发期工程名 smem_mm_ar_inline_reduce flavor 11，收尾规整改名
//  SmemMmIar310p3 / smem_mm_iar310p3；协议逻辑与本仓锁定版本逐字一致）。
//
// 结构（一次调用 = 一批子组，子组粒度 commRows 行，rows 为 256 对齐的任意
// [1024,4096]）：
//   AIC 侧（block 0..7）：8 核纯矩阵乘座位生产——MM 结果写对称池 work 区，
//                         尾组实属性行数不足 1024 时 pad 切片核整片跳过，
//                         组完成通知（座位）无条件发布；
//   AIV 侧（block 8）   ：三游标流水调度器——RS+AG 半交换（REDUCE=ADD_FP16
//                         非 owned 半区入对端 + GATHER=COPY 回传，硬件归约），
//                         A 游标座位齐即发 seatUp、B/C 游标按对端旗标非阻塞
//                         探测回填，同子组发射序 REDUCE→RDONE→GATHER→GDONE；
//   AIV 侧（block 9..12）：4 条尾部拷出车道——逐子组等对端 GDONE 单旗标后
//                         把收敛后的 work 纯拷出至 y；
//   AIV 侧（block 13）  ：完成通知消费者核——按确定计数排空本端发出的全部
//                         完成邮件（全 kernel 唯一 wait_at 点），调度器全程
//                         零 wait_at。
// 无标量 GM 访问：全部 GM 流量经 MTE（DataCopy + UB 暂存）。
#include "kernel_operator.h"
#include "lib/matmul/matmul_intf.h"
#include "smem_mm_iar_310p3_tiling.h"
#include "smem_shm_aicore_base_sdma.h"

namespace {
using namespace AscendC;

constexpr uint32_t kCores = 8;
constexpr uint32_t kWidth = 2048;
constexpr uint64_t kRowBytes = kWidth * 2ULL;
constexpr uint32_t kCommRows = 1024;
constexpr uint32_t kMmCoreRows = kCommRows / kCores;
constexpr uint64_t kGroupBytes = kCommRows * kRowBytes;
constexpr uint64_t kIbBytes = 8192;
constexpr uint64_t kDataBytes = 16ULL << 20;
constexpr uint64_t kScratchOff = kDataBytes;
constexpr uint64_t kScratchBytes = kDataBytes;
constexpr uint64_t kCtrlSrcOff = kScratchOff + kScratchBytes;
constexpr uint64_t kCtrlDstOff = kCtrlSrcOff + 64;
constexpr uint64_t kPeerRdyOff = kCtrlSrcOff + 512;
constexpr uint64_t kReadyOff = kCtrlSrcOff + 1024;
constexpr uint64_t kSumOff = kCtrlSrcOff + 2048;
constexpr uint32_t kTraceBytes = 4096;
constexpr uint32_t kTraceU64 = kTraceBytes / 8;
constexpr uint64_t kTraceMagic = 0x4D325431ULL;
constexpr uint32_t kTraceHeader = 16;
constexpr uint32_t kTraceCoreStride = 16;
constexpr uint32_t kTraceSchedBase = kTraceHeader + kCores * kTraceCoreStride;
enum Phase : uint32_t { REDUCE = 2, GATHER = 3, SEATUP = 5, RDONE = 6, GDONE = 7 };

__aicore__ inline uint64_t DeviceTag(uint32_t phase, uint32_t epoch, uint32_t group, uint32_t packet)
{
    return (uint64_t{phase} << 36) | (uint64_t{epoch} << 12) | (uint64_t{group} << 6) | packet;
}

__aicore__ inline void Flush(__gm__ uint8_t* valid)
{
    GlobalTensor<int32_t> fence;
    fence.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t*>(valid), 16);
    DataCacheCleanAndInvalid<int32_t, CacheLine::ENTIRE_DATA_CACHE>(fence);
}

__aicore__ inline void Check(bool ok, __gm__ uint8_t* status, uint64_t code)
{
    if (ok) return;
    *reinterpret_cast<__gm__ volatile uint64_t*>(status) = code;
    Flush(status);
    Trap();
}

// Poll one 8-byte epoch flag slot in my work region through MTE until it
// holds `epoch` (the slot is written by the peer's 8B COPY signal_at with
// the epoch payload staged through ctrlSrc). Returns spin iterations.
__aicore__ inline uint32_t PollFlag(LocalTensor<int64_t>& poll, __gm__ uint8_t* slot,
                                    uint32_t epoch, __gm__ uint8_t* status, uint64_t code)
{
    GlobalTensor<int64_t> pf;
    pf.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(slot), 8);
    uint32_t spins = 0, delay = 32;
    while (true) {
        DataCopy(poll, pf, 8);
        if (static_cast<uint32_t>(poll.GetValue(0)) == epoch) return spins;
        Check(++spins < 100000000, status, code);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & 255u) == 0 && delay < 1024) delay <<= 1;
    }
}

// Non-blocking probe of one 8B epoch flag slot — the scheduler event loop
// re-probes every gate each iteration instead of blocking on one.
__aicore__ inline bool ProbeFlag(LocalTensor<int64_t>& poll, __gm__ uint8_t* slot, uint32_t epoch)
{
    GlobalTensor<int64_t> pf;
    pf.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(slot), 8);
    DataCopy(poll, pf, 8);
    return static_cast<uint32_t>(poll.GetValue(0)) == epoch;
}

// Probe a range of seat slots of one MM group (are the producer cores done?).
__aicore__ inline uint32_t ProbeSeats(LocalTensor<int64_t>& poll, __gm__ uint8_t* flags,
                                      uint32_t group, uint32_t firstSlot, uint32_t count,
                                      uint32_t epoch)
{
    GlobalTensor<int64_t> fg;
    fg.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(flags + uint64_t{group} * 512), 64);
    DataCopy(poll, fg, 64);
    uint32_t ahead = 0;
    for (uint32_t c = 0; c < count; ++c) {
        if (static_cast<uint32_t>(poll.GetValue((firstSlot + c) * 8)) == epoch) ++ahead;
    }
    return ahead;
}

// REDUCE leg — ADD_FP16 packets of my non-owned half into the peer's same
// offset, then the RDONE flag. Per-sender FIFO ⟹ the peer seeing RDONE[sg]
// implies the whole leg landed（规避序的发射侧）。
// 去 pad 契约：len = 子组实字节（末子组可部分行），half = len/2。
__aicore__ inline void IssueReduceLeg(__gm__ uint8_t* res, __gm__ uint8_t* work, __gm__ uint8_t* peer,
                                       __gm__ uint8_t* status, uint32_t sg, uint64_t subBytes,
                                       uint64_t len, uint64_t packet, uint32_t rank, uint32_t epoch)
{
    const uint64_t half = len / 2;
    const uint64_t rOff = uint64_t{sg} * subBytes + uint64_t{1 - rank} * half;
    for (uint64_t b = 0; b < half; b += packet) {
        const uint64_t n = half - b < packet ? half - b : packet;
        Check(smem_shm_sdma_signal_at(res, work + rOff + b, peer + rOff + b, n,
              DeviceTag(REDUCE, epoch, sg, static_cast<uint32_t>(b / packet)),
              SMEM_SHM_SDMA_OP_ADD_FP16), status, 102);
    }
    Check(smem_shm_sdma_signal_at(res, work + kCtrlSrcOff, peer + kPeerRdyOff + uint64_t{sg} * 64,
          8, DeviceTag(RDONE, epoch, sg, 0), SMEM_SHM_SDMA_OP_COPY), status, 101);
}

// GATHER leg — COPY packets of my owned (already final) half back to the
// peer, then the GDONE flag（发射侧门 = 对端 RDONE，调用处探测）.
// 去 pad 契约：len = 子组实字节（末子组可部分行），half = len/2。
__aicore__ inline void IssueGatherLeg(__gm__ uint8_t* res, __gm__ uint8_t* work, __gm__ uint8_t* peer,
                                       __gm__ uint8_t* status, uint32_t sg, uint64_t subBytes,
                                       uint64_t len, uint64_t packet, uint32_t rank, uint32_t epoch)
{
    const uint64_t half = len / 2;
    const uint64_t gOff = uint64_t{sg} * subBytes + uint64_t{rank} * half;
    for (uint64_t b = 0; b < half; b += packet) {
        const uint64_t n = half - b < packet ? half - b : packet;
        Check(smem_shm_sdma_signal_at(res, work + gOff + b, peer + gOff + b, n,
              DeviceTag(GATHER, epoch, sg, static_cast<uint32_t>(b / packet)),
              SMEM_SHM_SDMA_OP_COPY), status, 102);
    }
    Check(smem_shm_sdma_signal_at(res, work + kCtrlSrcOff, peer + kReadyOff + uint64_t{sg} * 64,
          8, DeviceTag(GDONE, epoch, sg, 0), SMEM_SHM_SDMA_OP_COPY), status, 101);
}

// 去 pad 契约：MM 组 g 的实属性行数（尾组 < 1024；rows%256 ⟹ 实属性行数为
// 128 行切片的整数倍 ⇒ 每核切片整实或整 pad，pad 切片核可整片跳过）。
__aicore__ inline uint32_t RealRowsInGroup(uint32_t rows, uint32_t g)
{
    const uint32_t lo = g * kCommRows;
    return lo + kCommRows <= rows ? kCommRows : rows - lo;
}

// 去 pad 契约：通信子组 s 的实字节（末子组可部分行；rows%256 ⟹ 1MiB 整倍，
// 半区 halfLen ≥ 512KiB 且 512 对齐、四分 laneLen ≥ 256KiB）。
__aicore__ inline uint64_t SubBytesOf(uint32_t rows, uint64_t subBytes, uint32_t s)
{
    const uint64_t total = uint64_t{rows} * kRowBytes;
    const uint64_t lo = uint64_t{s} * subBytes;
    return lo + subBytes <= total ? subBytes : total - lo;
}
}

extern "C" __global__ __aicore__ void smem_mm_iar310p3(GM_ADDR x, GM_ADDR w, GM_ADDR y,
                                                        GM_ADDR workspace, GM_ADDR tiling)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_VECTOR_CORE);
    REGISTER_TILING_DEFAULT(SmemMmIar310p3TilingData);
    GET_TILING_DATA(td, tiling);

    __gm__ uint8_t* gva = reinterpret_cast<__gm__ uint8_t*>(td.gva);
    const uint64_t symm = td.symm;
    const uint32_t rank = td.rank;
    const uint32_t epoch = td.epoch;
    const uint32_t packet = td.packet;
    const uint32_t rows = td.rows;
    const uint32_t aicCount = td.aicCount;
    __gm__ uint8_t* status = reinterpret_cast<__gm__ uint8_t*>(td.statusPtr);
    __gm__ uint8_t* trace = reinterpret_cast<__gm__ uint8_t*>(td.tracePtr);

    if (td.enabled == 0) {
        // 会话预热波：不执行任何协议动作，直接返回（调用方在首波前以
        // enabled=0 发射一次并做双端 barrier，用于初始化设备侧资源）。
        return;
    }

    // 契约：rows%256==0 + 1024≤rows≤4096（4096 = work 区 16MiB 上限）。
    // 硬件归约协议支持部分尾组/末子组（去 pad 契约，原生 M 直传）。
    Check((td.commRows == kCommRows || td.commRows == kCommRows / 2) &&
          rows % 256 == 0 && rows >= kCommRows && rows <= 4 * kCommRows &&
          symm >= kSumOff + kDataBytes &&
          packet >= 512 && packet <= (4u << 20) && packet % 512 == 0, status, 1);

    __gm__ uint8_t* work = gva + symm * rank;
    __gm__ uint8_t* peer = gva + symm * (1 - rank);
    __gm__ uint8_t* flags = reinterpret_cast<__gm__ uint8_t*>(td.ibPtr) + kIbBytes;
    const uint32_t groups = (rows + kCommRows - 1) / kCommRows;   // 尾组可部分行

#ifdef __DAV_CUBE__
    const uint32_t core = static_cast<uint32_t>(GetBlockIdx());
    if (core >= kCores) return;
    using MvA = MatmulType<TPosition::GM, CubeFormat::ND, float16_t, false>;
    using MvB = MatmulType<TPosition::GM, CubeFormat::NZ, float16_t, true>;
    using MvC = MatmulType<TPosition::GM, CubeFormat::ND, float16_t, false>;
    using MvBias = MatmulType<TPosition::GM, CubeFormat::ND, float, false>;
    constexpr MatmulQuantParams kQuant = {false, false};
    constexpr MatmulBatchParams kBatch = {false, BatchMode::NONE};
    constexpr MatmulFuncParams kFunc = {false, true, false, false, 0,
        IterateOrder::UNDEF, ScheduleType::INNER_PRODUCT, true, true};
    constexpr MatmulShapeParams kShape = {kMmCoreRows, kWidth, kWidth, 128, 256, 64};
    constexpr MatmulConfig kCfg = GetMMConfig<MatmulConfigMode::CONFIG_NORM>(kShape, kQuant, kBatch, kFunc);
    constexpr static MatmulApiStaticTiling kTiling = GetMatmulApiTiling<MvA, MvB, MvC, MvBias>(kCfg, Impl::L1_SIZE);
    TPipe pipe;
    TBuf<TPosition::VECCALC> mmBuf;
    TBuf<TPosition::VECCALC> ioBuf;
    constexpr uint32_t kAddElems = 8192;
    constexpr uint32_t kAddBytes = kAddElems * 2;
    static_assert(TOTAL_UB_SIZE > 128 * 256 * 2 * 2 + 512 + 3 * 2 * kAddBytes,
                  "Insufficient UB for the basicM128 Matmul");
    pipe.InitBuffer(mmBuf, TOTAL_UB_SIZE - 512 - 3 * 2 * kAddBytes);
    pipe.InitBuffer(ioBuf, 256);
    TCubeTiling runtime{};
    runtime.usedCoreNum = kCores;
    Matmul<MvA, MvB, MvC, MvBias, kTiling> mm;
    REGIST_MATMUL_OBJ(&pipe, GetSysWorkSpacePtr(), mm, &runtime);
    mm.SetLocalWorkspace(mmBuf.Get<uint8_t>());
    LocalTensor<int64_t> io = ioBuf.Get<int64_t>();
    GlobalTensor<int64_t> tg;
    tg.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    GlobalTensor<float16_t> wg;
    wg.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(w), kWidth * kWidth);
    constexpr uint32_t kSliceRows = kMmCoreRows;
    constexpr uint64_t kSliceBytes = kSliceRows * kRowBytes;
    for (uint32_t g = 0; g < groups; ++g) {
        const uint64_t off = g * kGroupBytes + core * kSliceBytes;
        // 去 pad 契约：尾组实属性行数 < 1024 时 pad 切片核跳 MM（x 无 pad 行，
        // 读越界）；座位无条件发布（pad 座位缺席即死等）。
        if (core * kMmCoreRows < RealRowsInGroup(rows, g)) {
            GlobalTensor<float16_t> ag, cg;
            ag.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(x + off), kSliceRows * kWidth);
            cg.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + off), kSliceRows * kWidth);
            mm.SetTensorA(ag, false);
            mm.SetTensorB(wg, true);
            mm.SetOrgShape(kSliceRows, kWidth, kWidth, kWidth, kWidth);
            mm.IterateAll(cg);
            Flush(work + off);
        }
        io.SetValue(0, static_cast<int64_t>(epoch));
        GlobalTensor<int64_t> slot;
        slot.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(flags + uint64_t{g * kCores + core} * 64), 8);
        DataCopy(slot, io, 8);
        io.SetValue(8 + g, static_cast<int64_t>(0xA1C00000ULL | (uint64_t{core} << 8) | g));
    }
    DataCopy(tg[kTraceHeader + core * kTraceCoreStride], io[8], kTraceCoreStride);
#else
    (void)x;
    (void)w;
    (void)workspace;
    const uint32_t globalIdx = static_cast<uint32_t>(GetBlockIdx());
    const uint32_t localIdx = globalIdx >= aicCount ? globalIdx - aicCount : 0;
    // AIV 角色布局：block 8 = 三游标流水调度器，block 9..12 = 4 条尾部拷出
    // 车道，block 13 = 完成通知消费者核（唯一 wait_at 排空点），block 14 空闲
    // 退出。BlockDim = 15。
    if (localIdx > 5u) return;
    TPipe pipe;
    TBuf<TPosition::VECCALC> ioBuf;
    pipe.InitBuffer(ioBuf, 1024);
    LocalTensor<int64_t> io = ioBuf.Get<int64_t>();
    LocalTensor<int64_t> poll = io[0];
    LocalTensor<int64_t> stamps = io[64];
    GlobalTensor<int64_t> tg;
    tg.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    __gm__ uint8_t* res = smem_shm_sdma_reserved(gva);
    // Communication granularity: sub-groups of commRows rows inside each
    // 1024-row MM group (kCommRows/subRows subs per MM group).
    const uint32_t subRows = td.commRows;
    const uint64_t subBytes = uint64_t{subRows} * kRowBytes;
    const uint32_t nSub = (rows + subRows - 1) / subRows;   // 末子组可部分行
    const uint32_t subsPerGroup = kCommRows / subRows;
    const uint32_t slotsPerSub = kCores / subsPerGroup;
    if (localIdx == 5) {
        // ------------- 完成通知消费者核（block 13）-------------
        // 排空从调度器剥离：调度器长时间泡在 signal_at 背压自旋（请求环
        // 64 槽）与门探测里，无人排空到达环会持续堆积（packet 下限 512B 档
        // 邮件数远超环深）。本核只 wait_at，按确定计数排干本端发出的全部
        // 完成邮件后退出（完成邮件落发起方 res 到达环）：
        //   每子组 seatUp 1 + REDUCE k + RDONE 1 + GATHER k + GDONE 1
        //   （k = ceil(half/packet)）。
        // 纯计数、零内容依赖（纪律：协议推进只认 GM 旗标，邮件只丢弃）；
        // 批次尾排干 = 到达环清零，跨批次邮箱状态干净；本核 = 全 kernel
        // 唯一 wait_at 消费者，调度器全程零 wait_at。
        // 去 pad 契约：邮件数逐子组按实字节累计（末子组部分行，包数少）。
        uint32_t total = 0;
        for (uint32_t s = 0; s < nSub; ++s) {
            const uint64_t lenS = SubBytesOf(rows, subBytes, s);
            total += 2 * static_cast<uint32_t>((lenS / 2 + packet - 1) / packet) + 3;
        }
        for (uint32_t i = 0; i < total; ++i) {
            const auto m = smem_shm_sdma_wait_at(res);
            Check(m.status == SMEM_SHM_SDMA_MAIL_OK, status, 121);
        }
        return;
    }
    if (localIdx >= 1) {
        // ------------- 尾部拷出车道 l（block 9..12）-------------
        // 半交换 ADD_FP16 硬件归约收敛后，work 即全量和（对端 REDUCE ADD 落
        // 我 owned 半区 + GATHER 回传我非 owned 半区，见调度器注释），尾部纯
        // 拷出 work→y。GM→GM 纯拷贝必须经 MTE2→Vec→MTE3 三队列链，Adds(x, 0)
        // 标量加 0 即纯拷出。门 = 对端 GDONE[sg] 单旗标（per-sender FIFO ⟹
        // GDONE 落地 ⟹ 其 RDONE 与全部 REDUCE/GATHER 包先落我区 ⟹ 两半区
        // 同终值）。
        const uint32_t lane = localIdx - 1;
        constexpr uint32_t kTailLanes = 4;
        // 去 pad 契约：子组实字节四分（末子组部分行），基址 = 子组内车道偏移。
        constexpr uint32_t kAddElems = 8192;
        constexpr uint32_t kAddBytes = kAddElems * 2;
        TQue<TPosition::VECIN, 2> in0;
        TQue<TPosition::VECOUT, 2> outQ;
        pipe.InitBuffer(in0, 2, kAddBytes);
        pipe.InitBuffer(outQ, 2, kAddBytes);
        uint64_t spinSum = 0;
        for (uint32_t sg = 0; sg < nSub; ++sg) {
            // GDONE 单门化——per-sender FIFO 保证对端 GDONE[sg] 落地 ⟹ 其
            // RDONE[sg] 与全部 REDUCE 包先落我区（同发射序）。
            spinSum += PollFlag(poll, work + kReadyOff + uint64_t{sg} * 64,
                                epoch, status, 155);
            // 去 pad 契约：子组实字节四分（末子组部分行），基址 = 子组内车道偏移。
            const uint64_t laneBytes = SubBytesOf(rows, subBytes, sg) / kTailLanes;
            const uint64_t base = uint64_t{sg} * subBytes + uint64_t{lane} * laneBytes;
            GlobalTensor<float16_t> a, dst;
            a.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + base),
                              laneBytes / 2);
            dst.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                reinterpret_cast<uint64_t>(y) + base), laneBytes / 2);
            for (uint64_t off = 0; off < laneBytes; off += kAddBytes) {
                LocalTensor<float16_t> la = in0.AllocTensor<float16_t>();
                DataCopy(la, a[off / 2], kAddElems);
                in0.EnQue(la);
                LocalTensor<float16_t> va = in0.DeQue<float16_t>();
                LocalTensor<float16_t> out = outQ.AllocTensor<float16_t>();
                Adds(out, va, (float16_t)0, kAddElems);
                outQ.EnQue(out);
                LocalTensor<float16_t> result = outQ.DeQue<float16_t>();
                DataCopy(dst[off / 2], result, kAddElems);
                outQ.FreeTensor(result);
                in0.FreeTensor(va);
            }
        }
        poll.SetValue(0, static_cast<int64_t>(spinSum));
        DataCopy(tg[kTraceSchedBase + 64 + lane], poll, 8);
        return;
    }
    // ------------- 通信调度器（block 8）：三游标流水 -------------
    // 传输语义 = RS+AG 半交换（REDUCE=ADD_FP16 非 owned 半区入对端 +
    // GATHER=COPY 回传，硬件归约身份）。收敛机制为三游标事件循环，消除逐子组
    // 串行依赖（每子组暴露 ~2 个握手往返 × nSub）：
    //   A 游标：我方该子组座位齐 → seatUp credit 先行（绝不阻塞对端进度）；
    //   B 游标：对端 seatUp[b] 可见 → REDUCE[b]+RDONE[b]（b < a：源=我已算
    //           完的非 owned 半区）；
    //   C 游标：对端 RDONE[c] 可见 → GATHER[c]+GDONE[c]（c < b：源=我
    //           owned 半区已终值）。
    // 铁律：同子组发射序 REDUCE→RDONE→GATHER→GDONE（per-sender FIFO 公理，
    // 尾部车道 GDONE⟹RDONE 推理依赖它）；空转时重探全部门而非阻塞单一门
    //（防错门长等）。排水移交消费者核，调度器全程零 wait_at（纪律：协议推进
    // 只认 GM 旗标，wait_at 不出现在任何发射路径上）。
    // stamps：各游标发射时点的空转轮数（自上次进展起算 = 门滞后诊断量）。
    GlobalTensor<int64_t> myCtrl;
    myCtrl.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kCtrlSrcOff), 8);
    io.SetValue(0, static_cast<int64_t>(epoch));
    DataCopy(myCtrl, io, 8);
    stamps.SetValue(0, static_cast<int64_t>(kTraceMagic));
    stamps.SetValue(1, static_cast<int64_t>(rows));
    stamps.SetValue(2, static_cast<int64_t>(subRows));
    stamps.SetValue(3, static_cast<int64_t>(rank));
    stamps.SetValue(4, static_cast<int64_t>(epoch));
    stamps.SetValue(5, static_cast<int64_t>(nSub));
    uint32_t aCur = 0, bCur = 0, cCur = 0;
    uint32_t spins = 0, delay = 32;
    while (cCur < nSub) {
        uint32_t did = 0;
        if (aCur < nSub) {
            const uint32_t g = aCur / subsPerGroup;
            const uint32_t first = (aCur % subsPerGroup) * slotsPerSub;
            if (ProbeSeats(poll, flags, g, first, slotsPerSub, epoch) == slotsPerSub) {
                // seatUp credit 先行：座位齐即发，不等对端任何状态。
                Check(smem_shm_sdma_signal_at(res, work + kCtrlSrcOff,
                      peer + kReadyOff + 512 + uint64_t{aCur} * 64, 8,
                      DeviceTag(SEATUP, epoch, aCur, 0), SMEM_SHM_SDMA_OP_COPY), status, 101);
                stamps.SetValue(8 + aCur * 4 + 0, static_cast<int64_t>(spins));
                ++aCur;
                ++did;
            }
        }
        if (bCur < aCur && ProbeFlag(poll, work + kReadyOff + 512 + uint64_t{bCur} * 64,
                                     epoch)) {
            // 对端 seatUp[b] 可见：对端 work 该子组就绪，我的 ADD 可落。
            IssueReduceLeg(res, work, peer, status, bCur, subBytes,
                           SubBytesOf(rows, subBytes, bCur), packet, rank, epoch);
            stamps.SetValue(8 + bCur * 4 + 1, static_cast<int64_t>(spins));
            ++bCur;
            ++did;
        }
        if (cCur < bCur && ProbeFlag(poll, work + kPeerRdyOff + uint64_t{cCur} * 64,
                                     epoch)) {
            // 对端 RDONE[c] 可见：我 owned 半区已终值，GATHER 源可读。
            IssueGatherLeg(res, work, peer, status, cCur, subBytes,
                           SubBytesOf(rows, subBytes, cCur), packet, rank, epoch);
            stamps.SetValue(8 + cCur * 4 + 2, static_cast<int64_t>(spins));
            ++cCur;
            ++did;
        }
        if (did > 0) {
            spins = 0;
            delay = 32;
            continue;
        }
        Check(++spins < 100000000, status, 157);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & 255u) == 0 && delay < 1024) delay <<= 1;
    }
    Check(smem_shm_sdma_quiet_at(res), status, 105);
    DataCopy(tg[0], stamps, 8);
    DataCopy(tg[kTraceSchedBase], stamps[8], 8 + nSub * 4);
#endif
}
