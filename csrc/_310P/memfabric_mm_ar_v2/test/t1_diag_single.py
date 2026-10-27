# SPDX-License-Identifier: Apache-2.0
# T1 单卡 MM 隔离：对照 mf310p_v2_diag_nzdirect_mm_async 与 F.linear。
# 验证原始 NZ 沿 N 分核、ND 直接输出的 SetOrgShape/SetSingleShape。
# 不创建 MemFabric 池，不使用分布式、IB 或 SDMA，仅验证矩阵计算。
# 设置 DIAG_PRESYNC=1 可缓解首次 MTE3 输出丢失，
# 详见 T1_NZ_DIRECT_REPORT.md；否则新缓冲结果可能不可靠。
# 用法：python t1_diag_single.py [m ...]，默认测试边界 M。
# T1 单卡 MM 隔离：比较原始 NZ 直接 ND 与 F.linear。
# 不使用 MemFabric、分布式、IB 或 SDMA，
# 仅验证 SetOrgShape/SetSingleShape 的计算合同。
import ctypes
import os
import sys

import torch
import torch.nn.functional as F
import torch_npu

ACL_FORMAT_FRACTAL_NZ = 29
try:
    import vllm_ascend.vllm_ascend_C as _ascend_ext

    _DEFAULT_LIB = _ascend_ext.__file__
except Exception:
    _DEFAULT_LIB = "/tmp/opencode/v2/libmf310p_v2_trial_device.so"
DEVLIB = os.environ.get("V2_DEVLIB", _DEFAULT_LIB)


def main():
    dev = int(os.environ.get("DIAG_DEV", "0"))
    torch.npu.set_device(dev)
    device = torch.device(f"npu:{dev}")
    lib = ctypes.CDLL(DEVLIB)
    lib.mf310p_v2_diag_nzdirect_mm_async.argtypes = [
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    lib.mf310p_v2_diag_nzdirect_mm_async.restype = ctypes.c_int
    stream = torch.npu.current_stream().npu_stream

    torch.manual_seed(4242)
    w = torch.randn(2048, 2048, dtype=torch.float16, device=device)
    w_nz = torch_npu.npu_format_cast(w, ACL_FORMAT_FRACTAL_NZ)

    def templ(m):
        for t in (16, 32, 64, 128, 256):
            if m <= t:
                return t
        raise ValueError(m)

    # 先测安全 M，最后再测已知可能崩溃的 m=256、mm=1。
    plan = []
    for m in [int(a) for a in sys.argv[1:]] or [16, 32, 64, 128, 1, 2, 3, 15, 17, 31, 33, 63, 65, 127, 129, 254, 255]:
        for mm in (1, 2, 4, 8):
            plan.append((m, mm))
    for mm in (8, 4, 2, 1):
        plan.append((256, mm))

    n_pass = n_fail = 0
    for m, mm in plan:
        t = templ(m)
        torch.manual_seed(1200 + m)
        x = torch.randn(m, 2048, dtype=torch.float16, device=device)
        ref = F.linear(x, w)
        y = torch.zeros(m, 2048, dtype=torch.float16, device=device)
        if os.environ.get("DIAG_PRESYNC"):
            # T0 BUG#3 风险探针：首次向新分配的 y 写入 MTE3 数据前，
            # 显式同步以缓解首写丢失。
            torch.npu.current_stream().synchronize()
        # 符号空启动两次（enable=0）后再真实运行。
        for _ in range(2):
            rc = lib.mf310p_v2_diag_nzdirect_mm_async(x.data_ptr(), w_nz.data_ptr(), y.data_ptr(), t, m, mm, 0, stream)
            assert rc == 0, f"launch rc={rc} (warm)"
        rc = lib.mf310p_v2_diag_nzdirect_mm_async(x.data_ptr(), w_nz.data_ptr(), y.data_ptr(), t, m, mm, 1, stream)
        assert rc == 0, f"launch rc={rc}"
        torch.npu.current_stream().synchronize()
        if torch.equal(y, ref):
            n_pass += 1
            print(f"DIAGNZ\tT={t}\tm={m}\tmm={mm}\tPASS\ty_abs_max={y.float().abs().max().item():.3f}", flush=True)
        else:
            diff = (y.float() - ref.float()).abs()
            bad = diff > 0
            n_fail += 1
            print(
                f"DIAGNZ\tT={t}\tm={m}\tmm={mm}\tFAIL\t"
                f"max_abs={diff.max().item():.4f}\t"
                f"n_bad={int(bad.sum().item())}\t"
                f"y_abs_max={y.float().abs().max().item():.3f}\t"
                f"ref_abs_max={ref.float().abs().max().item():.3f}",
                flush=True,
            )
    print(f"DIAGNZ\tsummary\tpass={n_pass}\tfail={n_fail}", flush=True)


if __name__ == "__main__":
    main()
