import argparse
import ctypes

import torch_npu  # noqa: F401
import memfabric_hybrid as mf
from memfabric_hybrid import shm


def check(ret, api):
    if ret:
        raise RuntimeError(f"{api} ret={ret}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rank", type=int, choices=(0, 1))
    parser.add_argument("device", type=int)
    parser.add_argument("store")
    parser.add_argument("library")
    parser.add_argument("rounds", type=int)
    parser.add_argument("bytes", type=int)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10000 or not 64 <= args.bytes <= 4 * 1024 * 1024 or args.bytes % 64:
        raise ValueError("rounds must be 1..10000; bytes must be a multiple of 64 in 64..4194304")
    check(mf.initialize(), "initialize")
    check(mf.set_log_level(1), "set_log_level")
    config = shm.ShmConfig()
    config.start_store = args.rank == 0
    config.init_timeout = config.create_timeout = config.operation_timeout = 60
    check(shm.initialize(args.store, 2, args.rank, args.device, config), "shm.initialize")
    pool = shm.create(0, 2, args.rank, 32 * 1024 * 1024, shm.ShmDataOpType.SDMA, 0)
    library = ctypes.CDLL(args.library)
    library.prepare_controls.argtypes = [ctypes.c_void_p]
    library.prepare_controls.restype = ctypes.c_int
    library.run_exchange.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint64, ctypes.c_uint32]
    library.run_exchange.restype = ctypes.c_int
    check(library.prepare_controls(pool.gva), "prepare_controls")
    # Initialize control cells before either rank can send its first notification.
    check(pool.barrier(), "control initialization barrier")
    check(library.run_exchange(pool.gva, args.rank, args.bytes, args.rounds), "run_exchange")
    # Protect pool lifetime after all device-side protocol rounds have completed.
    check(pool.barrier(), "teardown barrier")
    check(pool.destroy(), "pool.destroy")
    shm.uninitialize()
    mf.uninitialize()


if __name__ == "__main__":
    main()
