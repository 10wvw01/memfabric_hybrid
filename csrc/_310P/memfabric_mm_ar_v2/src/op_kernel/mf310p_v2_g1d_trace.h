#pragma once
// G1-D 精准取证：只记录 wait 已返回的邮件。不要在本函数主动读取到达环。
// 复用 G1-C Core0 独占的 64B 状态槽，避免新分配/新 ABI。
// +0 阶段、+8 当前代际、+16 采集代际、+24 邮件序号、
// +32 目标地址、+40 长度、+48 标签、+56 打包(高32位chunk,低32位status)。
#if defined(MF310P_V2_G1D_TRACE) && !defined(MF310P_V2_G1C_TRACE)
#error "G1-D requires G1-C per-core trace"
#endif
#ifdef MF310P_V2_G1D_TRACE
__aicore__ inline void G1dWaitResult(
    __gm__ uint8_t* status, uint32_t core, uint32_t chunk, const smem_shm_sdma_mail_t& mail)
{
    if (core != 0) return;
    __gm__ uint8_t* slot = status + kG1cTraceOffset +
                          static_cast<uint64_t>(core) * kG1cTraceSlotBytes;
    // 最后写采集代际；Host 只在代际一致时解释。
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 24) = mail.seq;
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 32) = mail.dst;
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 40) = mail.len;
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 48) = mail.imm;
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 56) =
        (static_cast<uint64_t>(chunk) << 32) | static_cast<uint32_t>(mail.status);
    *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 16) =
        *reinterpret_cast<__gm__ volatile uint64_t*>(slot + 8);
    CleanLines(slot, kG1cTraceSlotBytes);
}
#else
__aicore__ inline void G1dWaitResult(
    __gm__ uint8_t*, uint32_t, uint32_t, const smem_shm_sdma_mail_t&) {}
#endif
