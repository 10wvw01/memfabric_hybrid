import argparse
import ctypes
import os
from pathlib import Path

import torch_npu  # noqa: F401
import memfabric_hybrid as mf
from memfabric_hybrid import shm


def check(ret, api):
    if ret != 0:
        raise RuntimeError(f"{api} ret={ret}")


def validate_install(args):
    package = Path(mf.__file__).resolve()
    if not package.is_relative_to(Path(args.prefix).resolve()):
        raise RuntimeError(f"unexpected imported package: {package}")
    if os.environ.get("MF_SDMA_ORCH_DIR"):
        raise RuntimeError("MF_SDMA_ORCH_DIR must be unset for automatic discovery")
    import _pymf_hybrid

    extension = Path(_pymf_hybrid.__file__).resolve()
    if not extension.is_relative_to(Path(args.prefix).resolve()):
        raise RuntimeError(f"unexpected imported extension: {extension}")
    for asset in ("libmf_sdma_orch.so", "libmf_sdma_orch.json", "libmf_sdma_orch_launch.json"):
        if not (package.parent / "hybm" / "aicpu_kernel" / asset).is_file():
            raise RuntimeError(f"missing installed AICPU asset: {asset}")
    rounds = int(os.environ.get("ROUNDS", "100"))
    if rounds <= 0:
        raise ValueError("ROUNDS must be positive")
    print(f"IMPORT package={package} extension={extension}", flush=True)
    return rounds


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rank", type=int)
    parser.add_argument("device", type=int)
    parser.add_argument("store")
    parser.add_argument("library")
    parser.add_argument("prefix")
    args = parser.parse_args()
    rounds = validate_install(args)
    check(mf.initialize(), "initialize")
    check(mf.set_log_level(1), "log level")
    config = shm.ShmConfig()
    config.start_store = args.rank == 0
    config.init_timeout = config.create_timeout = config.operation_timeout = 60
    check(shm.initialize(args.store, 2, args.rank, args.device, config), "shm.initialize")
    pool = shm.create(0, 2, args.rank, 32 * 1024 * 1024, shm.ShmDataOpType.SDMA, 0)
    library = ctypes.CDLL(args.library)
    library.run_round.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t, ctypes.c_uint]
    library.run_round.restype = ctypes.c_int
    library.prepare_round.argtypes = library.run_round.argtypes
    library.prepare_round.restype = ctypes.c_int
    for iteration in range(rounds):
        check(library.prepare_round(pool.gva, args.rank, 131072, iteration), "prepare_round")
        check(pool.barrier(), "barrier before")
        check(library.run_round(pool.gva, args.rank, 131072, iteration), "run_round")
        check(pool.barrier(), "barrier after")
    check(pool.destroy(), "pool.destroy")
    shm.uninitialize()
    mf.uninitialize()
    print(f"PASS rank={args.rank} bytes=131072 rounds={rounds} full_payload_checked=1 cleanup_complete=1", flush=True)


if __name__ == "__main__":
    main()
