"""Run under mpirun after building libpypto_coexist.so."""

import ctypes
import faulthandler
import os
import pathlib
import sys
import traceback


def main():
    faulthandler.dump_traceback_later(30, repeat=True)
    rank = int(os.environ["OMPI_COMM_WORLD_RANK"])
    device = int(os.environ["TEST_DEVICE_BASE"]) + rank
    os.environ["TILE_FWK_DEVICE_ID"] = str(device)
    print(f"rank={rank}: import PyPTO dependencies", flush=True)
    import torch
    import torch_npu  # noqa: F401
    import pypto

    torch.npu.set_device(device)

    @pypto.frontend.jit(runtime_options={"run_mode": pypto.RunMode.NPU})
    def add_kernel(
        x: pypto.Tensor([...], pypto.DT_FP32),
        y: pypto.Tensor([...], pypto.DT_FP32),
        out: pypto.Tensor([...], pypto.DT_FP32),
    ):
        pypto.set_vec_tile_shapes(1, 4, 1, 64)
        out[:] = x + y

    @ctypes.CFUNCTYPE(ctypes.c_int)
    def run_pypto():
        try:
            x = torch.rand((1, 4, 1, 64), device=f"npu:{device}")
            y = torch.rand_like(x)
            out = torch.empty_like(x)
            add_kernel(x, y, out)
            torch.testing.assert_close(out.cpu(), (x + y).cpu())
            print(f"PASS PyPTO rank={rank}", flush=True)
            return 0
        except Exception:
            traceback.print_exc()
            return 1

    bridge = ctypes.CDLL(
        str(pathlib.Path(__file__).resolve().parent / "build/libpypto_coexist.so"), mode=ctypes.RTLD_GLOBAL
    )
    bridge.mf_pypto_coexist.argtypes = [ctypes.c_void_p, ctypes.c_int]
    bridge.mf_pypto_coexist.restype = ctypes.c_int
    assert bridge.mf_pypto_coexist(run_pypto, int(sys.argv[1])) == 0
    torch.npu.synchronize()
    faulthandler.cancel_dump_traceback_later()
    print(f"PASS coexist order={sys.argv[1]} rank={rank}", flush=True)


if __name__ == "__main__":
    main()
