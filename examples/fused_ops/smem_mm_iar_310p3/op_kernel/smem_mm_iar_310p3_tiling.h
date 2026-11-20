#ifndef SMEM_MM_IAR310_P3_TILING_H
#define SMEM_MM_IAR310_P3_TILING_H
#include <cstdint>

// ---- 池内布局推导（唯一事实源） ----
// 外部只负责申请一块足够大的对称内存池并把 gva/symm 传入算子，池内区域
// 划分完全由本文件推导，不再散落在 kernel 魔法常量与 README 外部契约里：
//   op_host 据此在 tiling 期校验池下限（不满足即 GRAPH_FAILED，kernel 不发射）；
//   kernel  据此取各区域地址（constexpr，运行期零开销）；
//   xops    据此导出 pool_floor_bytes() 等查询函数，会话封装层程序化建池，
//           外部无需阅读任何布局文档。
// 单侧布局（地址 = 池基址 + rank*symm + 偏移）：
//   [0, kDataBytes)          work 区 —— 本端矩阵乘结果与收敛终值
//   [kScratchOff, kCtrlOff)  scratch 区 —— 预留（本协议不使用，为与 ar 版
//                            布局兼容保留同名常量）
//   [kCtrlOff + 0..64)       控制位 —— 本端源控制字
//   [kCtrlOff + 64..)         控制位 —— 对端目的控制字
//   [kPeerRdyOff + sg*64)     peerRdy —— 对端写回的 RDONE 标记
//   [kReadyOff + sg*64)       ready —— 对端写回的 GDONE 标记；
//   [kReadyOff + 512 + sg*64) ready —— 对端写回的 seatUp 标记（硬件归约
//                            版专用：调度器 A 游标的组就绪通知槽段）
//   [kSeatsOff, kPoolFloor)  seats —— 组完成通知数组
// 行数契约：rows 为 kRowsAlign 的整数倍且在 [kMinRows, kMaxRows] 内；
// kMaxRows 由 work 区容量决定（kMaxRows * kRowBytes == kDataBytes）。
constexpr uint32_t kWidth = 2048;               // K == N 固定宽
constexpr uint32_t kMaxRows = 4096;             // 单次调用最大行数
constexpr uint64_t kRowBytes = kWidth * 2ULL;   // fp16 行字节数
constexpr uint32_t kRowsAlign = 256;            // 行数对齐要求
constexpr uint32_t kMinRows = 1024;             // 最小行数（1 个通信子组）
constexpr uint64_t kDataBytes = kMaxRows * kRowBytes;        // work 区 16MiB
constexpr uint64_t kScratchOff = kDataBytes;
constexpr uint64_t kScratchBytes = kDataBytes;               // scratch 区 16MiB
constexpr uint64_t kCtrlOff = kScratchOff + kScratchBytes;   // 32MiB 起
constexpr uint64_t kCtrlDstOff = kCtrlOff + 64;              // 对端目的控制字
constexpr uint64_t kPeerRdyOff = kCtrlOff + 512;
constexpr uint64_t kReadyOff = kCtrlOff + 1024;
constexpr uint64_t kSeatsOff = kCtrlOff + 4096;              // 组完成通知数组
constexpr uint64_t kSeatsBytes = 4096;         // 需求 4 组 × 512B，2 倍余量
constexpr uint64_t kPoolFloor = kSeatsOff + kSeatsBytes;      // 建池下限

// smem_mm_iar_310p3（算子类型 SmemMmIar310p3）的 tiling 数据结构。
// 字段顺序必须与 op_host TilingFunc 的属性写入顺序一致，也必须与
// xops 层 aclnnSmemMmIar310p3 的实参顺序一致，三者任何一处改动都要同步。
// 只保留 7 个运行期真正变化的属性；其余协议参数为 kernel 编译期常量：
//   对端地址由 gva 推算（对称窗口下两端 gva 相同，会话层校验基址一致）；
//   SDMA 分包粒度 1MiB / 通信子组行数 1024 / 矩阵乘核数 8（本文件常量段）；
//   组完成通知数组在池内 seats 区（kSeatsOff 推导），会话层不分配通知缓冲；
//   预热职责在会话层：首发射存在冷首发竞态窗口（见 README 调用纪律），
//   会话须先发射暖机调度批次。
struct SmemMmIar310p3TilingData {
    uint64_t gva;        // 本端对称池在设备虚拟地址空间中的起始地址
    uint64_t symm;       // 对称池单侧字节数（两 rank 各占 symm 连续区间）
    uint64_t statusPtr;  // kernel 错误码写出地址（校验失败时写入，宿主可读）
    uint64_t tracePtr;   // 调度批次计时与诊断数据写出地址（每核一段）
    uint32_t rank;       // 本进程 rank 编号（0 或 1），决定取池的哪一侧
    uint32_t epoch;      // 调度批次编号，随每次真实调用递增，用于标记防重
    uint32_t rows;       // 本次矩阵乘的行数 M，kRowsAlign 对齐且在
                         // [kMinRows, kMaxRows] 内
};

#endif // SMEM_MM_IAR310_P3_TILING_H
