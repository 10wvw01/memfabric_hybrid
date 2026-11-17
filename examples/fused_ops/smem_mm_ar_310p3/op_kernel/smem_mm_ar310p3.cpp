// smem_mm_ar310p3 on dav-m200（Ascend 310P3）: TP2 MatMul + AllReduce 通算融合算子
//（本地归约版。研发期工程名 smem_mm_ar_inline_reduce flavor 10，收尾规整改名
//  SmemMmAr310p3 / smem_mm_ar310p3；协议逻辑与本仓锁定版本逐字一致）。
//
// 结构（一次调用 = 一批 1024 行的调度批次，rows 为 256 对齐的任意 [1024,4096]）：
//   AIC 侧（block 0..7）：8 核纯矩阵乘座位生产——MM 结果写对称池 work 区，
//                         尾组实属性行数不足 1024 时 pad 切片核整片跳过，
//                         组完成通知（座位）无条件发布；
//   AIV 侧（block 8）   ：通信调度器——逐组等本端座位齐 → 把本端组数据全量
//                         COPY 进对端 scratch 区 → 发 8 字节就绪旗标 → 排空
//                         本端完成通知 → 等对端旗标（对端组已落入我 scratch）；
//   AIV 侧（block 9..12）：4 条本地归约加法车道——逐组轮询对端 peerRdy 旗标 →
//                         work+scratch 相加 → 直写 y。
// 无标量 GM 访问：全部 GM 流量经 MTE（DataCopy + UB 暂存）。
#include "kernel_operator.h"
#include "lib/matmul/matmul_intf.h"
#include "smem_mm_ar_310p3_tiling.h"
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
enum Phase : uint32_t { READY = 1, REDUCE = 2, GATHER = 3 };

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

struct Transport {
    __gm__ uint8_t *res, *work, *peer, *status;
    uint32_t epoch, packet;

    __aicore__ inline void Send2(uint32_t phase, uint32_t group, uint64_t srcOff, uint64_t dstOff,
                                 uint64_t bytes, smem_shm_sdma_op_t op)
    {
        for (uint64_t b = 0; b < bytes; b += packet) {
            const uint64_t n = bytes - b < packet ? bytes - b : packet;
            Check(smem_shm_sdma_signal_at(res, work + srcOff + b, peer + dstOff + b, n,
                  DeviceTag(phase, epoch, group, static_cast<uint32_t>(b / packet)), op), status, 102);
        }
    }
};

// Poll a range of seat slots of one MM group through MTE (no scalar GM
// reads). Slot k of group g lives at flags + g*512 + k*64; its epoch value
// is the first u32 of the slot. Returns poll iterations spent waiting.
__aicore__ inline uint32_t WaitSeats(LocalTensor<int64_t>& poll, __gm__ uint8_t* flags, uint32_t group,
                                     uint32_t firstSlot, uint32_t count,
                                     uint32_t epoch, __gm__ uint8_t* status, uint64_t code)
{
    GlobalTensor<int64_t> fg;
    fg.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(flags + uint64_t{group} * 512), 64);
    uint32_t spins = 0, delay = 32;
    while (true) {
        DataCopy(poll, fg, 64);
        uint32_t ok = 1;
        for (uint32_t c = 0; c < count; ++c) {
            if (static_cast<uint32_t>(poll.GetValue((firstSlot + c) * 8)) != epoch) {
                ok = 0;
                break;
            }
        }
        if (ok) return spins;
        Check(++spins < 100000000, status, code);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & 255u) == 0 && delay < 1024) delay <<= 1;
    }
}

// 去 pad 契约：MM 组 g 的实属性行数（尾组 < 1024；rows%256 ⟹ 实属性行数为
// 128 行切片的整数倍 ⇒ 每核切片整实或整 pad，pad 切片核可整片跳过）。
__aicore__ inline uint32_t RealRowsInGroup(uint32_t rows, uint32_t g)
{
    const uint32_t lo = g * kCommRows;
    return lo + kCommRows <= rows ? kCommRows : rows - lo;
}

// 去 pad 契约：MM 组 g 的实字节（组基址 = g*kGroupBytes 不变，仅缩长度）。
__aicore__ inline uint64_t GroupBytesOf(uint32_t rows, uint32_t g)
{
    return uint64_t{RealRowsInGroup(rows, g)} * kRowBytes;
}
}

extern "C" __global__ __aicore__ void smem_mm_ar310p3(GM_ADDR x, GM_ADDR w, GM_ADDR y,
                                                       GM_ADDR workspace, GM_ADDR tiling)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_VECTOR_CORE);
    REGISTER_TILING_DEFAULT(SmemMmAr310p3TilingData);
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
    // 本地归约协议支持部分尾组/末子组（去 pad 契约，原生 M 直传）。
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
    // AIV 角色布局：block 8 = 通信调度器，block 9..12 = 4 条本地归约加法车道
    //（block 13/14 空闲退出）。BlockDim = 15。
    if (localIdx >= 5u) return;
    TPipe pipe;
    TBuf<TPosition::VECCALC> ioBuf;
    pipe.InitBuffer(ioBuf, 1024);
    LocalTensor<int64_t> io = ioBuf.Get<int64_t>();
    LocalTensor<int64_t> poll = io[0];
    LocalTensor<int64_t> stamps = io[64];
    GlobalTensor<int64_t> tg;
    tg.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    Transport t{smem_shm_sdma_reserved(gva), work, peer, status, epoch, packet};
    if (localIdx >= 1) {
        // ------------- 本地归约加法车道 l（block 9..12）-------------
        // 车道 l 负责每组内 256 行切片：轮询对端 peerRdy[g]（对端组 g 数据已
        // 落入我 scratch；早于调度器的 ready 确认门）→ work+scratch 相加 →
        // 直写 y。AIC 侧为纯 MM——归约加法从 AIC 加尾移到 AIV 车道，AIC 腿
        // 不再被通信链尾的等门自旋钉死。
        // 调度批次完成语义：kernel 退出 = 全部 block（含车道）完成 ⇒ 跨批次
        // 安全性与 AIC 加尾结构等同（E2E 拓扑 AR + 同流串行既有论证不变）。
        const uint32_t lane = localIdx - 1;
        constexpr uint32_t kTailLanes = 4;
        constexpr uint32_t kAddElems = 8192;
        constexpr uint32_t kAddBytes = kAddElems * 2;
        TQue<TPosition::VECIN, 2> in0, in1;
        TQue<TPosition::VECOUT, 2> outQ;
        pipe.InitBuffer(in0, 2, kAddBytes);
        pipe.InitBuffer(in1, 2, kAddBytes);
        pipe.InitBuffer(outQ, 2, kAddBytes);
        uint64_t spinSum = 0;
        for (uint32_t g = 0; g < groups; ++g) {
            GlobalTensor<int64_t> pf;
            pf.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(
                work + kPeerRdyOff + uint64_t{g} * 64), 8);
            uint32_t spins = 0, delay = 32;
            while (true) {
                DataCopy(poll, pf, 8);
                if (static_cast<uint32_t>(poll.GetValue(0)) == epoch) break;
                Check(++spins < 100000000, status, 152);
                for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
                if ((spins & 255u) == 0 && delay < 1024) delay <<= 1;
            }
            spinSum += spins;
            // 组 g 实字节四分（尾组部分行），基址 = 组内车道偏移不变。
            const uint64_t laneBytes = GroupBytesOf(rows, g) / kTailLanes;
            const uint64_t base = g * kGroupBytes + uint64_t{lane} * laneBytes;
            GlobalTensor<float16_t> a, b, dst;
            a.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + base),
                              laneBytes / 2);
            b.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                work + kScratchOff + base), laneBytes / 2);
            dst.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                reinterpret_cast<uint64_t>(y) + base), laneBytes / 2);
            for (uint64_t off = 0; off < laneBytes; off += kAddBytes) {
                LocalTensor<float16_t> la = in0.AllocTensor<float16_t>();
                LocalTensor<float16_t> lb = in1.AllocTensor<float16_t>();
                DataCopy(la, a[off / 2], kAddElems);
                DataCopy(lb, b[off / 2], kAddElems);
                in0.EnQue(la); in1.EnQue(lb);
                LocalTensor<float16_t> va = in0.DeQue<float16_t>();
                LocalTensor<float16_t> vb = in1.DeQue<float16_t>();
                LocalTensor<float16_t> out = outQ.AllocTensor<float16_t>();
                Add(out, va, vb, kAddElems);
                outQ.EnQue(out);
                LocalTensor<float16_t> result = outQ.DeQue<float16_t>();
                DataCopy(dst[off / 2], result, kAddElems);
                outQ.FreeTensor(result); in0.FreeTensor(va); in1.FreeTensor(vb);
            }
        }
        poll.SetValue(0, static_cast<int64_t>(spinSum));
        DataCopy(tg[kTraceSchedBase + 64 + lane], poll, 8);
        return;
    }
    // ------------- 通信调度器（block 8）：全交换协议 -------------
    // 每端把整组数据 COPY 进对端 scratch 区（与对端 MM 无排序冲突：字节不
    // 相交），归约加法在本地车道执行。无 READY 确认、无 GATHER 回传、无远端
    // ADD。完成通知只落发送侧，故「对端组已落入我 scratch」信号 = 数据包后
    // 紧随的 8 字节旗标搬运（orch 按发送方保序）；加法车道轮询该旗标。
    const uint32_t myPhase = rank == 0 ? REDUCE : GATHER;
    GlobalTensor<int64_t> myCtrl;
    myCtrl.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kCtrlSrcOff), 8);
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t spins = WaitSeats(poll, flags, g, 0, kCores, epoch, status, 131);
        stamps.SetValue(8 + g * 4 + 0, static_cast<int64_t>(spins));
        io.SetValue(0, static_cast<int64_t>(epoch));
        DataCopy(myCtrl, io, 8);
        // 组 g 实字节（整除契约下 == kGroupBytes，行为不变）。
        t.Send2(myPhase, g, g * kGroupBytes, kScratchOff + g * kGroupBytes,
                GroupBytesOf(rows, g), SMEM_SHM_SDMA_OP_COPY);
        Check(smem_shm_sdma_signal_at(t.res, work + kCtrlSrcOff, peer + kPeerRdyOff + uint64_t{g} * 64,
              8, DeviceTag(READY, epoch, g, 0), SMEM_SHM_SDMA_OP_COPY), status, 102);
        // 排空发送侧完成通知直到本组旗标通知出现（数据自通知按 FIFO 先到；
        // 其余交错内容无害，消费后丢弃）。
        while (true) {
            const auto m = smem_shm_sdma_wait_at(t.res);
            Check(m.status == SMEM_SHM_SDMA_MAIL_OK, status, 121);
            if (m.imm == DeviceTag(READY, epoch, g, 0)) break;
        }
        // 轮询对端写入我区的旗标：对端组数据已在我 scratch。
        GlobalTensor<int64_t> pf;
        pf.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kPeerRdyOff + uint64_t{g} * 64), 8);
        uint32_t spins2 = 0, delay = 32;
        while (true) {
            DataCopy(poll, pf, 8);
            if (static_cast<uint32_t>(poll.GetValue(0)) == epoch) break;
            Check(++spins2 < 100000000, status, 152);
            for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
            if ((spins2 & 255u) == 0 && delay < 1024) delay <<= 1;
        }
        GlobalTensor<int64_t> rslot;
        rslot.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kReadyOff + uint64_t{g} * 64), 8);
        DataCopy(rslot, io, 8);
    }
    Check(smem_shm_sdma_quiet_at(t.res), status, 105);
    DataCopy(tg[0], stamps, 8);
    DataCopy(tg[kTraceSchedBase], stamps[8], 8 + groups * 4);
#endif
}
