// smem_mm_ar310p3：TP2（两卡各持权重一半）下的 MatMul + AllReduce 融合算子，
// 本地归约版，运行于 dav-m200（Ascend 310P3）。
// 一次调用处理 rows 行（256 对齐，[1024,4096]），每 1024 行为一个通信组：
//   block 0..7   Cube 矩阵乘段（fused_matmul）：结果写池内 work 区，本核
//                切片完成后无条件发布组完成通知；
//   block 8      通信调度器：逐组等本端通知齐 → 组数据复制进对端 scratch →
//                发 8B 就绪标记 → 确认对端标记已到；
//   block 9..12  归约加法分支：逐组轮询对端就绪标记 → work 与 scratch 相加
//                → 直接写 y；
//   block 13/14  空闲退出（BlockDim=15）。
// 文件分层：smem_mm_ar_310p3_tiling.h 池布局（唯一事实源）/
// smem_mm_ar_310p3_protocol.h 协议常量 / smem_mm_ar_310p3_transport.h 传输
// 原语 / 本文件三侧编排。全局内存流量一律经 MTE 搬运，不做标量访问。
#include "kernel_operator.h"
#include "lib/matmul/matmul_intf.h"
#include "smem_mm_ar_310p3_transport.h"

// 显式文件级 using：AscendC 名字由头文件提供，协议名字由 ar310p3_kernel 提供。
using namespace AscendC;
using namespace ar310p3_kernel;

namespace ar310p3_kernel {

#ifdef __DAV_CUBE__
// Cube 侧（block 0..7）矩阵乘段：逐组把本核 128 行切片与全量权重相乘写进
// work 区。行数对齐/范围、池下限等契约由 op_host 在 tiling 期前置校验并拒绝
// 发射，kernel 侧不重复检查。
__aicore__ inline void fused_matmul(const SmemMmAr310p3TilingData& tilingData, GM_ADDR x, GM_ADDR w)
{
    // rank 0/1 各占池的一半连续区间：work = 池 + rank*symmBytes。
    __gm__ uint8_t* poolGva = reinterpret_cast<__gm__ uint8_t*>(tilingData.gva);
    const uint64_t symmBytes = tilingData.symm;
    const uint32_t epoch = tilingData.epoch;
    const uint32_t rows = tilingData.rows;
    __gm__ uint8_t* trace = reinterpret_cast<__gm__ uint8_t*>(tilingData.tracePtr);
    __gm__ uint8_t* work = poolGva + symmBytes * tilingData.rank;
    __gm__ uint8_t* seats = work + kSeatsOff;
    const uint32_t groups = (rows + kCommRows - 1) / kCommRows;   // 末尾组可不足整组

    const uint32_t core = static_cast<uint32_t>(GetBlockIdx());
    if (core >= kCores) return;
    // 静态配置：A 激活 ND、B 权重 NZ 且转置、C 输出 ND，每核切片 128×2048×2048；
    // tiling 编译期由 Matmul 组件按 L1 容量生成，运行时只填 usedCoreNum。
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
    // mmBuf 是 Matmul 工作区，ioBuf 是组完成通知写入暂存。
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
    LocalTensor<int64_t> ub = ioBuf.Get<int64_t>();
    GlobalTensor<int64_t> traceGm;
    traceGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    GlobalTensor<float16_t> weightGm;
    weightGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(w), kWidth * kWidth);
    constexpr uint32_t kSliceRows = kMmCoreRows;
    constexpr uint64_t kSliceBytes = kSliceRows * kRowBytes;
    // 逐组矩阵乘：本核切片位于组基址偏移 core*kSliceBytes。
    for (uint32_t group = 0; group < groups; ++group) {
        const uint64_t offset = group * kGroupBytes + core * kSliceBytes;
        // 去填充契约：切片完全落在末尾组填充区则跳过（x 无填充行，读会越界）；
        // 组完成通知在下方无条件发布——调度器固定等 8 个通知，缺一个即死等。
        if (core * kMmCoreRows < real_rows_in_group(rows, group)) {
            GlobalTensor<float16_t> actGm, mmOutGm;
            actGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(x + offset), kSliceRows * kWidth);
            mmOutGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + offset), kSliceRows * kWidth);
            mm.SetTensorA(actGm, false);
            mm.SetTensorB(weightGm, true);
            mm.SetOrgShape(kSliceRows, kWidth, kWidth, kWidth, kWidth);
            mm.IterateAll(mmOutGm);
            // 写完必须刷缓存，调度器与归约分支才能经 MTE 读到新数据。
            flush(work + offset);
        }
        // 组完成通知：本批次 epoch 经 MTE 写进本核槽位（槽布局见 protocol.h）。
        ub.SetValue(0, static_cast<int64_t>(epoch));
        GlobalTensor<int64_t> slotGm;
        slotGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(
            seats + uint64_t{group * kCores + core} * kFlagSlotBytes), kFlagSlotQwords);
        DataCopy(slotGm, ub, kFlagSlotQwords);
        // 诊断标记：记录本核参与了哪一组（写 trace）。
        ub.SetValue(8 + group, static_cast<int64_t>(0xA1C00000ULL | (uint64_t{core} << 8) | group));
    }
    DataCopy(traceGm[kTraceHeader + core * kTraceCoreStride], ub[8], kTraceCoreStride);
}
#else
// Vector 侧（block 8..12）AllReduce 段。
__aicore__ inline void allreduce(const SmemMmAr310p3TilingData& tilingData, GM_ADDR y, GM_ADDR workspace)
{
    (void)workspace;
    // 本端 work = 池 + rank*symmBytes，对端 peer = 池 + (1-rank)*symmBytes；
    // status 供运行期失败写错误码（仅 Vector 侧使用）。
    __gm__ uint8_t* poolGva = reinterpret_cast<__gm__ uint8_t*>(tilingData.gva);
    const uint64_t symmBytes = tilingData.symm;
    const uint32_t rank = tilingData.rank;
    const uint32_t epoch = tilingData.epoch;
    const uint32_t rows = tilingData.rows;
    __gm__ uint8_t* status = reinterpret_cast<__gm__ uint8_t*>(tilingData.statusPtr);
    __gm__ uint8_t* trace = reinterpret_cast<__gm__ uint8_t*>(tilingData.tracePtr);
    __gm__ uint8_t* work = poolGva + symmBytes * rank;
    __gm__ uint8_t* peer = poolGva + symmBytes * (1 - rank);
    __gm__ uint8_t* seats = work + kSeatsOff;
    const uint32_t groups = (rows + kCommRows - 1) / kCommRows;   // 末尾组可不足整组

    const uint32_t globalIdx = static_cast<uint32_t>(GetBlockIdx());
    const uint32_t localIdx = globalIdx >= kCores ? globalIdx - kCores : 0;
    if (localIdx >= 5u) return;
    // 公共准备：轮询暂存 poll、时间戳暂存 stamps、SDMA 传输对象。
    TPipe pipe;
    TBuf<TPosition::VECCALC> ioBuf;
    pipe.InitBuffer(ioBuf, 1024);
    LocalTensor<int64_t> ub = ioBuf.Get<int64_t>();
    LocalTensor<int64_t> poll = ub[0];
    LocalTensor<int64_t> stamps = ub[64];
    GlobalTensor<int64_t> traceGm;
    traceGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    // SDMA 上下文：新版 memfabric 只允许 block<8 提交 SDMA，而调度器在 Vector
    // block 8。Cube 侧 block 0..7 不做 SDMA，其队列空闲，故把上下文 block 映射
    // 到 0（队列下标仅按 block 划分，通知靠显式 8B 槽地址传递，不依赖 block 路由）。
    smem_shm_sdma_ctx_t sdmaCtx = smem_shm_sdma_ctx(poolGva);
    sdmaCtx.block = 0;
    Transport transport{sdmaCtx, work, peer, status, epoch, kPacket};
    if (localIdx >= 1) {
        // ---------- 归约加法分支（block 9..12）----------
        // 分支 lane 各负责每组数据的四分之一：轮询对端 peerRdy 标记 → work 与
        // scratch 相加 → 写 y。Cube 只做矩阵乘，归约在 Vector 侧并行，Cube
        // 时间不被通信链尾部的等标记空转拖长。
        // 正确性：本分支对本端 work 无直接门——依赖双方矩阵乘同构 + 对端就绪
        // 标记天然滞后于其搬运链（见标记 ⇒ 对端数据先落地）。
        const uint32_t lane = localIdx - 1;
        constexpr uint32_t kTailLanes = 4;
        constexpr uint32_t kAddElems = 8192;
        constexpr uint32_t kAddBytes = kAddElems * 2;
        // workIn/peerIn 搬运输入块，outQ 存相加结果，各配 2 级队列做流水。
        TQue<TPosition::VECIN, 2> workIn, peerIn;
        TQue<TPosition::VECOUT, 2> outQ;
        pipe.InitBuffer(workIn, 2, kAddBytes);
        pipe.InitBuffer(peerIn, 2, kAddBytes);
        pipe.InitBuffer(outQ, 2, kAddBytes);
        uint64_t spinSum = 0;
        // 逐组：等对端标记 → 分块相加写 y。y 与池内 work 布局逐字节对齐，
        // 偏移可直接复用；分支内偏移从组基址起固定，末尾组只缩长度。
        for (uint32_t group = 0; group < groups; ++group) {
            spinSum += wait_peer_rdy(poll, work, group, epoch, status);
            const uint64_t laneBytes = group_bytes_of(rows, group) / kTailLanes;
            const uint64_t base = group * kGroupBytes + uint64_t{lane} * laneBytes;
            GlobalTensor<float16_t> workGm, peerGm, dstGm;
            workGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + base),
                              laneBytes / 2);
            peerGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                work + kScratchOff + base), laneBytes / 2);
            dstGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                reinterpret_cast<uint64_t>(y) + base), laneBytes / 2);
            // 每次搬 16KiB 进统一缓冲区，相加后直接写 y。
            for (uint64_t offset = 0; offset < laneBytes; offset += kAddBytes) {
                LocalTensor<float16_t> workTile = workIn.AllocTensor<float16_t>();
                LocalTensor<float16_t> peerTile = peerIn.AllocTensor<float16_t>();
                DataCopy(workTile, workGm[offset / 2], kAddElems);
                DataCopy(peerTile, peerGm[offset / 2], kAddElems);
                workIn.EnQue(workTile); peerIn.EnQue(peerTile);
                LocalTensor<float16_t> workVal = workIn.DeQue<float16_t>();
                LocalTensor<float16_t> peerVal = peerIn.DeQue<float16_t>();
                LocalTensor<float16_t> out = outQ.AllocTensor<float16_t>();
                Add(out, workVal, peerVal, kAddElems);
                outQ.EnQue(out);
                LocalTensor<float16_t> result = outQ.DeQue<float16_t>();
                DataCopy(dstGm[offset / 2], result, kAddElems);
                outQ.FreeTensor(result); workIn.FreeTensor(workVal); peerIn.FreeTensor(peerVal);
            }
        }
        // 本分支等待轮数写进 trace。
        poll.SetValue(0, static_cast<int64_t>(spinSum));
        DataCopy(traceGm[kTraceSchedBase + 64 + lane], poll, kFlagSlotQwords);
        return;
    }
    // ---------- 通信调度器（block 8）：全交换协议 ----------
    // 两端互把组数据复制进对端 scratch（写向对端的不同区域，字节不相交，
    // 不与对端矩阵乘互扰），无数据回传、无远端加法。"对端组数据已到我方
    // scratch"的信号 = 数据包之后紧跟的 8B 就绪标记（共队列 FIFO 保序，
    // 标记必晚于数据到达），归约分支轮询该标记即可。
    GlobalTensor<int64_t> ctrlSrcGm;
    ctrlSrcGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kCtrlOff), kFlagSlotQwords);
    for (uint32_t group = 0; group < groups; ++group) {
        // 等本端 8 核通知齐，轮数记入 trace。
        const uint32_t spins = wait_seats(poll, seats, group, 0, kCores, epoch, status,
                                         ProtocolError::GroupSeatsTimeout);
        stamps.SetValue(8 + group * 4 + 0, static_cast<int64_t>(spins));
        // epoch 写本端源控制字（诊断用，不参与协议）。
        ub.SetValue(0, static_cast<int64_t>(epoch));
        DataCopy(ctrlSrcGm, ub, kFlagSlotQwords);
        // 组数据复制进对端 scratch；随后单独发 8B 就绪标记。
        transport.send_group_copy(group, rows);
        transport.signal_peer_ready(group);
        // 对端标记已到我方 peerRdy 槽，本端归约分支随时可相加。
        wait_peer_rdy(poll, work, group, epoch, status);
        // 本端就绪标记写 ready 槽（诊断用，对端不读）。
        GlobalTensor<int64_t> readySlotGm;
        readySlotGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(
            work + kReadyOff + uint64_t{group} * kFlagSlotBytes), kFlagSlotQwords);
        DataCopy(readySlotGm, ub, kFlagSlotQwords);
    }
    // 排空队列确保全部已投递传输落地（源缓冲复用前）；trace 头写 magic，
    // 调度器轮数段入 trace。
    check(smem_shm_sdma_quiet_at(transport.ctx), status, ProtocolError::QuietFail);
    ub.SetValue(0, static_cast<int64_t>(kTraceMagic));
    DataCopy(traceGm[0], ub, kFlagSlotQwords);
    DataCopy(traceGm[kTraceSchedBase], stamps[8], 8 + groups * 4);
}
#endif

}  // namespace ar310p3_kernel

// kernel 入口。Cube/Vector 两侧由 __DAV_CUBE__ 宏在编译期分开，
// 各自只编译本侧函数。
extern "C" __global__ __aicore__ void smem_mm_ar310p3(GM_ADDR x, GM_ADDR w, GM_ADDR y,
                                                       GM_ADDR workspace, GM_ADDR tiling)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_VECTOR_CORE);
    REGISTER_TILING_DEFAULT(SmemMmAr310p3TilingData);
    GET_TILING_DATA(tilingData, tiling);
#ifdef __DAV_CUBE__
    fused_matmul(tilingData, x, w);
#else
    allreduce(tilingData, y, workspace);
#endif
}
