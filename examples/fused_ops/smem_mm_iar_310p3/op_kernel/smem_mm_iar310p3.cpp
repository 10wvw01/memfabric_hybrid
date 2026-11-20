// smem_mm_iar310p3：TP2（两卡各持权重一半）下的 MatMul + AllReduce 融合算子，
// 硬件归约版，运行于 dav-m200（Ascend 310P3）。
// 一次调用处理 rows 行（256 对齐，[1024,4096]），矩阵乘按 1024 行分组，
// 通信子组与矩阵乘组同粒度：
//   block 0..7   Cube 矩阵乘段（fused_matmul）：结果写池内 work 区，本核
//                切片完成后无条件发布组完成通知；
//   block 8      三游标流水调度器（allreduce）：REDUCE 把本端不拥有的半区
//                以 ADD_FP16 加到对端（SDMA 传输途中完成加法）、GATHER 把
//                本端已收敛的半区复制回对端；三个游标各自推进流水级；
//   block 9..12  尾部拷出分支：逐子组等对端 GDONE 标记后，把已收敛的 work
//                区数据拷贝到输出 y；
//   block 13     完成通知消费者核：按确定计数排空本端发出的全部 SDMA 完成
//                通知（全 kernel 唯一的阻塞等待点），调度器因此全程不阻塞；
//   block 14     空闲退出（BlockDim=15）。
// 文件分层：smem_mm_iar_310p3_tiling.h 池布局（唯一事实源）/
// smem_mm_iar_310p3_protocol.h 协议常量 / smem_mm_iar_310p3_transport.h 传输
// 原语 / 本文件三侧编排。全局内存流量一律经 MTE 搬运，不做标量访问。
#include "kernel_operator.h"
#include "lib/matmul/matmul_intf.h"
#include "smem_mm_iar_310p3_transport.h"

// 显式文件级 using：AscendC 名字由头文件提供，协议名字由 iar310p3_kernel 提供。
using namespace AscendC;
using namespace iar310p3_kernel;

namespace iar310p3_kernel {

#ifdef __DAV_CUBE__
// Cube 侧（block 0..7）矩阵乘段：逐组把本核 128 行切片与全量权重相乘写进
// work 区。行数对齐/范围、池下限等契约由 op_host 在 tiling 期前置校验并拒绝
// 发射，kernel 侧不重复检查。
__aicore__ inline void fused_matmul(const SmemMmIar310p3TilingData& tilingData, GM_ADDR x, GM_ADDR w)
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
        // 组完成通知在下方无条件发布——调度器固定按槽位数等齐，缺一个即死等。
        if (core * kMmCoreRows < real_rows_in_group(rows, group)) {
            GlobalTensor<float16_t> actGm, mmOutGm;
            actGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(x + offset), kSliceRows * kWidth);
            mmOutGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + offset), kSliceRows * kWidth);
            mm.SetTensorA(actGm, false);
            mm.SetTensorB(weightGm, true);
            mm.SetOrgShape(kSliceRows, kWidth, kWidth, kWidth, kWidth);
            mm.IterateAll(mmOutGm);
            // 写完必须刷缓存，调度器才能经 MTE 读到新数据。
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
// Vector 侧（block 8..13）AllReduce 段：三游标调度器 + 4 条尾部拷出分支 +
// 1 个完成通知消费者核。
__aicore__ inline void allreduce(const SmemMmIar310p3TilingData& tilingData, GM_ADDR y, GM_ADDR workspace)
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

    const uint32_t globalIdx = static_cast<uint32_t>(GetBlockIdx());
    const uint32_t localIdx = globalIdx >= kCores ? globalIdx - kCores : 0;
    if (localIdx > 5u) return;
    // 公共准备：轮询暂存 poll、时间戳暂存 stamps、SDMA 资源句柄。
    TPipe pipe;
    TBuf<TPosition::VECCALC> ioBuf;
    pipe.InitBuffer(ioBuf, 1024);
    LocalTensor<int64_t> ub = ioBuf.Get<int64_t>();
    LocalTensor<int64_t> poll = ub[0];
    LocalTensor<int64_t> stamps = ub[64];
    GlobalTensor<int64_t> traceGm;
    traceGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(trace), kTraceU64);
    __gm__ uint8_t* sdmaRes = smem_shm_sdma_reserved(poolGva);
    // 通信粒度：矩阵乘组内按子组行数切通信子组（子组与矩阵乘组同粒度，
    // subsPerGroup = 1、slotsPerSub = 8）。
    const uint32_t subRows = kSubRows;
    const uint64_t subBytes = uint64_t{subRows} * kRowBytes;
    const uint32_t subCount = (rows + subRows - 1) / subRows;   // 末子组可不足整子组
    const uint32_t subsPerGroup = kCommRows / subRows;
    const uint32_t slotsPerSub = kCores / subsPerGroup;
    if (localIdx == 5) {
        // ---------- 完成通知消费者核（block 13）----------
        // 把"排空完成通知"从调度器剥离：调度器要持续处理传输请求（请求环仅
        // 64 槽，写满会背压阻塞），若还自己等通知，到达环会不断堆积。本核
        // 按确定计数逐个取走本端发出的全部完成通知后退出：每子组数量 =
        // 组完成通知 1 + REDUCE 包 k + RDONE 1 + GATHER 包 k + GDONE 1
        //（k = 半区字节数按包长向上取整）。只数数量不看内容（协议推进只认
        // 全局内存标记，通知本身仅丢弃）；计数按子组实属字节数累计。
        uint32_t notifyTotal = 0;
        for (uint32_t subGroup = 0; subGroup < subCount; ++subGroup) {
            const uint64_t subLen = sub_bytes_of(rows, subBytes, subGroup);
            notifyTotal += 2 * static_cast<uint32_t>((subLen / 2 + kPacket - 1) / kPacket) + 3;
        }
        for (uint32_t i = 0; i < notifyTotal; ++i) {
            const auto notice = smem_shm_sdma_wait_at(sdmaRes);
            check(notice.status == SMEM_SHM_SDMA_MAIL_OK, status, ProtocolError::MailStatusNotOk);
        }
        return;
    }
    if (localIdx >= 1) {
        // ---------- 尾部拷出分支（block 9..12）----------
        // 半交换收敛后 work 区即两端之和：本端拥有的半区由对端 REDUCE 加法
        // 落值，本端不拥有的半区由对端 GATHER 回传。本分支只做纯拷贝
        // work→y（GM→GM 纯拷贝须走 MTE2 搬入 → 向量单元 → MTE3 搬出的
        // 三段队列链，用 Adds(x, 0) 实现等价拷出）。
        // 开始条件 = 对端 GDONE[subGroup]：同一发送方保序，GDONE 落地即
        // RDONE 与全部数据包都已先落地，两个半区均为终值。
        const uint32_t lane = localIdx - 1;
        constexpr uint32_t kTailLanes = 4;
        constexpr uint32_t kAddElems = 8192;
        constexpr uint32_t kAddBytes = kAddElems * 2;
        // workIn 搬 work 数据块，outQ 存拷出结果，各配 2 级队列做流水。
        TQue<TPosition::VECIN, 2> workIn;
        TQue<TPosition::VECOUT, 2> outQ;
        pipe.InitBuffer(workIn, 2, kAddBytes);
        pipe.InitBuffer(outQ, 2, kAddBytes);
        uint64_t spinSum = 0;
        // 逐子组：等 GDONE → 分块拷出。y 与池内 work 布局逐字节对齐，偏移
        // 可直接复用；分支内偏移从子组基址起固定，末子组只缩长度。
        for (uint32_t subGroup = 0; subGroup < subCount; ++subGroup) {
            spinSum += poll_flag(poll, work + kReadyOff + uint64_t{subGroup} * kFlagSlotBytes,
                                epoch, status, ProtocolError::GdoneWaitTimeout);
            const uint64_t laneBytes = sub_bytes_of(rows, subBytes, subGroup) / kTailLanes;
            const uint64_t base = uint64_t{subGroup} * subBytes + uint64_t{lane} * laneBytes;
            GlobalTensor<float16_t> workGm, dstGm;
            workGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(work + base),
                              laneBytes / 2);
            dstGm.SetGlobalBuffer(reinterpret_cast<__gm__ float16_t*>(
                reinterpret_cast<uint64_t>(y) + base), laneBytes / 2);
            // 每次搬 16KiB 进统一缓冲区，加 0 后写 y。
            for (uint64_t offset = 0; offset < laneBytes; offset += kAddBytes) {
                LocalTensor<float16_t> workTile = workIn.AllocTensor<float16_t>();
                DataCopy(workTile, workGm[offset / 2], kAddElems);
                workIn.EnQue(workTile);
                LocalTensor<float16_t> workVal = workIn.DeQue<float16_t>();
                LocalTensor<float16_t> out = outQ.AllocTensor<float16_t>();
                Adds(out, workVal, (float16_t)0, kAddElems);
                outQ.EnQue(out);
                LocalTensor<float16_t> result = outQ.DeQue<float16_t>();
                DataCopy(dstGm[offset / 2], result, kAddElems);
                outQ.FreeTensor(result);
                workIn.FreeTensor(workVal);
            }
        }
        // 本分支等待轮数写进 trace。
        poll.SetValue(0, static_cast<int64_t>(spinSum));
        DataCopy(traceGm[kTraceSchedBase + 64 + lane], poll, kFlagSlotQwords);
        return;
    }
    // ---------- 三游标流水调度器（block 8）----------
    // 推进机制是三个游标的事件循环，消除逐子组串行握手的往返等待：
    //   A 游标：本端该子组的组完成通知齐 → 立即向对端发 seatUp（只依赖本端
    //          进度，绝不因等对端而阻塞）；
    //   B 游标：对端 seatUp[b] 可见 → 发 REDUCE[b] + RDONE[b]（此时源数据 =
    //          我已算完的非拥有半区）；
    //   C 游标：对端 RDONE[c] 可见 → 发 GATHER[c] + GDONE[c]（本端拥有的半区
    //          已是终值，可安全回传）。
    // 发射顺序约束：同一子组按 REDUCE→RDONE→GATHER→GDONE 发出（尾部拷出
    // 分支"GDONE 落地即全部先落地"的推理依赖此顺序）；空转一轮把所有门
    // 重探一遍，不阻塞在单个门上。完成通知排空在消费者核，调度器任何
    // 发射路径都不等待。stamps 记录各游标发射时点的空转轮数（从上次进展
    // 起算，门滞后的诊断量）。
    GlobalTensor<int64_t> ctrlSrcGm;
    ctrlSrcGm.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t*>(work + kCtrlOff), kFlagSlotQwords);
    // 批次头（魔数/行数/子组行数/rank/批次号/子组数）写时间戳暂存区，
    // 随后与游标时间戳一起落 trace。
    ub.SetValue(0, static_cast<int64_t>(epoch));
    DataCopy(ctrlSrcGm, ub, kFlagSlotQwords);
    stamps.SetValue(0, static_cast<int64_t>(kTraceMagic));
    stamps.SetValue(1, static_cast<int64_t>(rows));
    stamps.SetValue(2, static_cast<int64_t>(subRows));
    stamps.SetValue(3, static_cast<int64_t>(rank));
    stamps.SetValue(4, static_cast<int64_t>(epoch));
    stamps.SetValue(5, static_cast<int64_t>(subCount));
    // 三游标事件循环：C 游标走完全部子组即整个交换完成。
    uint32_t aCur = 0, bCur = 0, cCur = 0;
    uint32_t spins = 0, delay = kBackoffStart;
    while (cCur < subCount) {
        uint32_t did = 0;
        if (aCur < subCount) {
            // A 门：本端子组 aCur 的组完成通知齐（非阻塞探测）→ 立即发 seatUp。
            const uint32_t group = aCur / subsPerGroup;
            const uint32_t firstSlot = (aCur % subsPerGroup) * slotsPerSub;
            if (probe_seats(poll, seats, group, firstSlot, slotsPerSub, epoch) == slotsPerSub) {
                check(smem_shm_sdma_signal_at(sdmaRes, work + kCtrlOff,
                      peer + kReadyOff + kSeatUpOff + uint64_t{aCur} * kFlagSlotBytes, 8,
                      device_tag(SEATUP, epoch, aCur, 0), SMEM_SHM_SDMA_OP_COPY), status,
                      ProtocolError::SignalMarkFail);
                stamps.SetValue(8 + aCur * 4 + 0, static_cast<int64_t>(spins));
                ++aCur;
                ++did;
            }
        }
        if (bCur < aCur && probe_flag(poll, work + kReadyOff + kSeatUpOff + uint64_t{bCur} * kFlagSlotBytes,
                                     epoch)) {
            // B 门：对端 seatUp[b] 可见——对端该子组矩阵乘已完成，
            // 我的 ADD 数据可以落到对端区域。
            issue_reduce_leg(sdmaRes, work, peer, status, bCur, subBytes,
                             sub_bytes_of(rows, subBytes, bCur), kPacket, rank, epoch);
            stamps.SetValue(8 + bCur * 4 + 1, static_cast<int64_t>(spins));
            ++bCur;
            ++did;
        }
        if (cCur < bCur && probe_flag(poll, work + kPeerRdyOff + uint64_t{cCur} * kFlagSlotBytes,
                                     epoch)) {
            // C 门：对端 RDONE[c] 可见——本端拥有的半区已是终值，
            // 可以作为 GATHER 的源回传。
            issue_gather_leg(sdmaRes, work, peer, status, cCur, subBytes,
                             sub_bytes_of(rows, subBytes, cCur), kPacket, rank, epoch);
            stamps.SetValue(8 + cCur * 4 + 2, static_cast<int64_t>(spins));
            ++cCur;
            ++did;
        }
        if (did > 0) {
            // 本轮有进展：空转计数与退避间隔复位，立即进入下一轮。
            spins = 0;
            delay = kBackoffStart;
            continue;
        }
        // 本轮无进展：空转计数加一并指数退避；死等保护见 kPollBudget。
        check(++spins < kPollBudget, status, ProtocolError::SchedLoopTimeout);
        for (uint32_t i = 0; i < delay; ++i) __asm__ __volatile__("" ::: "memory");
        if ((spins & (kBackoffStep - 1u)) == 0 && delay < kBackoffMax) delay <<= 1;
    }
    // 全部子组交换完成后排空队列，把批次头与游标时间戳写进 trace 区。
    check(smem_shm_sdma_quiet_at(sdmaRes), status, ProtocolError::QuietFail);
    DataCopy(traceGm[0], stamps, kFlagSlotQwords);
    DataCopy(traceGm[kTraceSchedBase], stamps[8], 8 + subCount * 4);
}
#endif

}  // namespace iar310p3_kernel

// kernel 入口。Cube/Vector 两侧由 __DAV_CUBE__ 宏在编译期分开，
// 各自只编译本侧函数。
extern "C" __global__ __aicore__ void smem_mm_iar310p3(GM_ADDR x, GM_ADDR w, GM_ADDR y,
                                                        GM_ADDR workspace, GM_ADDR tiling)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_VECTOR_CORE);
    REGISTER_TILING_DEFAULT(SmemMmIar310p3TilingData);
    GET_TILING_DATA(tilingData, tiling);
#ifdef __DAV_CUBE__
    fused_matmul(tilingData, x, w);
#else
    allreduce(tilingData, y, workspace);
#endif
}
