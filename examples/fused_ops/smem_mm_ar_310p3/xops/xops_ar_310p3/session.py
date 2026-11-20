"""SmemSession：smem_mm_ar_310p3 的会话封装层（随 xops wheel 发布）。

外部（vllm / m2 / op_bench）只与本类交互。此前外部为了正确使用算子必须
掌握的知识——建池尺寸下限、ctypes 建池调用序列、对称窗口几何校验、
status/trace 通道尺寸、epoch 单调递增纪律、算子属性传参顺序——全部收编
到本模块内部，外部调用面收敛为三件事：

    import xops_ar_310p3
    sess = xops_ar_310p3.SmemSession(device, rank, store_url=...)
    y = sess.launch(x, w_nz, rows)          # 也可传 out= 预分配输出
    sess.teardown()

约束不再以 README 文档形式交由外部执行，而是构造上不可违反：建池尺寸取
“64MiB 与算子导出的布局下限的较大者”（pool_floor_bytes() 与 kernel/
op_host 同源于 op_kernel/smem_mm_ar_310p3_tiling.h，不存在第二份手工
数字）；池几何两端基址一致性在会话建立时强校验；epoch 由封装层内管
（首次发射为 1001，每次发射 +1，线程锁保护）。

时序契约（外部义务，仅此两条）：
1. 对称池窗口基址由进程设备虚拟地址状态决定，两端 rank 必须在相同的
   分配序列下建池（vllm 在 worker 启动最早期、任何业务张量分配之前
   创建会话，即为此设计）。封装层内 status/trace 在建池之后分配，不破坏
   “池是首个设备分配”的时序。
2. 首次 kernel 发射存在冷首发竞态窗口：正式校验前应先发射一次
   不校验的暖机调度批次（vllm 会话以首调用逐位校验门兜底，腐化会响亮
   失败）。
"""
import ctypes
import os
import threading

import torch
import torch_npu

from . import xops_lib

# ---- 池契约（程序化导出，同源 op_kernel tiling.h 布局推导段） ----
POOL_FLOOR_BYTES = int(xops_lib.pool_floor_bytes())
KERNEL_WIDTH = int(xops_lib.kernel_width())
KERNEL_MAX_ROWS = int(xops_lib.kernel_max_rows())
KERNEL_MIN_ROWS = int(xops_lib.kernel_min_rows())
KERNEL_ROWS_ALIGN = int(xops_lib.kernel_rows_align())

# ---- host I/O 通道尺寸（非池内布局；kernel 侧写出区上限） ----
# status：kernel 校验失败时写入的 64 位错误码，64 字节足够；
# trace：调度批次计时与诊断区，kernel 按 4096 字节（512 个 int64）封顶
# 写入，通道过小会造成越界写，大一点无害。
STATUS_BYTES = 64
TRACE_WORDS = 512

# 会话默认建池尺寸：64MiB 与布局下限的较大者（布局下限 32MiB+8KiB，
# 64MiB 留一倍余量）。
DEFAULT_POOL_BYTES = max(64 << 20, POOL_FLOOR_BYTES)

_EPOCH_BASE = 1000
_SDMA_DATA_OP = 2          # smem_shm_create 的 dataOpType：SDMA 池
_RTLD_NOW = 0x2            # glibc：RTLD_NOW（ctypes 默认 RTLD_LAZY）


def _smem_lib_path() -> str:
    """定位 libmf_smem.so（MemFabric SDK 输出目录）。"""
    for root in (os.environ.get("MIXFABRIC_SDK_OUT", ""),
                 "/usr/local/memfabric_hybrid"):
        if not root:
            continue
        for sub in ("smem/lib64", "aarch64-linux/lib64", "lib64"):
            p = os.path.join(root, sub, "libmf_smem.so")
            if os.path.exists(p):
                return p
    raise RuntimeError(
        "libmf_smem.so not found; set MIXFABRIC_SDK_OUT to the memfabric "
        "output root (containing smem/lib64/libmf_smem.so)")


class _SmemApi:
    """memfabric smem 会话 API 的 ctypes 绑定。"""

    def __init__(self, so_path: str):
        self.lib = ctypes.CDLL(so_path, mode=ctypes.RTLD_GLOBAL | _RTLD_NOW)

        class SmemConfig(ctypes.Structure):
            _fields_ = [
                ("shmInitTimeout", ctypes.c_uint32),
                ("shmCreateTimeout", ctypes.c_uint32),
                ("controlOperationTimeout", ctypes.c_uint32),
                ("startConfigStoreServer", ctypes.c_bool),
                ("flags", ctypes.c_uint32),
            ]

        self.SmemConfig = SmemConfig
        lib = self.lib
        lib.smem_init.argtypes = [ctypes.c_uint32]
        lib.smem_shm_config_init.argtypes = [ctypes.POINTER(SmemConfig)]
        lib.smem_shm_init.argtypes = [ctypes.c_char_p, ctypes.c_uint32,
                                       ctypes.c_uint32, ctypes.c_uint16,
                                       ctypes.POINTER(SmemConfig)]
        lib.smem_shm_create.argtypes = [ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_uint32, ctypes.c_uint64,
                                         ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.POINTER(ctypes.c_void_p)]
        lib.smem_shm_create.restype = ctypes.c_void_p
        lib.smem_shm_sdma_get_workspace.argtypes = [ctypes.c_void_p]
        lib.smem_shm_sdma_get_workspace.restype = ctypes.c_void_p
        lib.smem_shm_get_symmetric_size.argtypes = [ctypes.c_void_p]
        lib.smem_shm_get_symmetric_size.restype = ctypes.c_uint64
        lib.smem_shm_control_allgather.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_void_p,
                                                    ctypes.c_uint32,
                                                    ctypes.c_void_p,
                                                    ctypes.c_uint32]
        lib.smem_shm_control_barrier.argtypes = [ctypes.c_void_p]
        lib.smem_shm_destroy.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        lib.smem_shm_uninit.argtypes = [ctypes.c_uint32]
        lib.smem_uninit.argtypes = []
        lib.smem_uninit.restype = None


class SmemSession:
    """smem 对称池会话：建池、几何校验、通道分配、epoch 内管、发射。

    一个进程一个会话（get_or_create 单例）；两侧 rank 各持一个进程，
    以相同分配序列各自建池。
    """

    _SESSION = None
    _SESSION_LOCK = threading.Lock()

    def __init__(self, device: int, rank: int, world_size: int = 2,
                 store_url: str = None, pool_bytes: int = None):
        if not 0 <= rank < world_size:
            raise ValueError(f"rank {rank} out of range [0, {world_size})")
        self.device = int(device)
        self.rank = int(rank)
        self.world_size = int(world_size)
        self._store_url = (store_url
                           or os.environ.get("SMEM_MM_310P3_STORE_URL",
                                             "tcp://127.0.0.1:8730"))
        self._pool_bytes = int(pool_bytes) if pool_bytes else DEFAULT_POOL_BYTES
        if self._pool_bytes < POOL_FLOOR_BYTES:
            raise ValueError(
                f"pool_bytes={self._pool_bytes} below operator pool floor "
                f"{POOL_FLOOR_BYTES} (derived from the kernel layout)")
        self._epoch = _EPOCH_BASE
        self._pool = None
        self._gva = 0
        self._peer_gva = 0
        self._symm = 0
        self._lock = threading.Lock()
        self._bootstrap()

    # ---- 生命周期 -------------------------------------------------------

    @classmethod
    def get_or_create(cls, device: int, rank: int, world_size: int = 2,
                      store_url: str = None,
                      pool_bytes: int = None) -> "SmemSession":
        with cls._SESSION_LOCK:
            if cls._SESSION is None:
                cls._SESSION = cls(device=device, rank=rank,
                                   world_size=world_size,
                                   store_url=store_url,
                                   pool_bytes=pool_bytes)
            return cls._SESSION

    def _bootstrap(self) -> None:
        import numpy as np  # 仅用于几何 allgather 的暂存缓冲

        # 库加载顺序纪律：先 libmf_smem.so、再 libascendcl.so，均
        # RTLD_NOW|RTLD_GLOBAL——与既有 m2 / serve 会话序列完全一致。
        api = _SmemApi(_smem_lib_path())
        self._api = api
        ctypes.CDLL("libascendcl.so", mode=ctypes.RTLD_GLOBAL | _RTLD_NOW)
        lib = api.lib

        rc = lib.smem_init(0)
        if rc != 0:
            raise RuntimeError(f"smem_init rc={rc}")
        cfg = api.SmemConfig()
        rc = lib.smem_shm_config_init(ctypes.byref(cfg))
        if rc != 0:
            raise RuntimeError(f"smem_shm_config_init rc={rc}")
        # rank 0 承担配置存储服务端角色，两端经 store_url 会合。
        cfg.startConfigStoreServer = self.rank == 0
        rc = lib.smem_shm_init(self._store_url.encode(), self.world_size,
                               self.rank, self.device, ctypes.byref(cfg))
        if rc != 0:
            raise RuntimeError(f"smem_shm_init rc={rc} (store_url="
                               f"{self._store_url})")

        base = ctypes.c_void_p()
        pool = lib.smem_shm_create(0, self.world_size, self.rank,
                                   self._pool_bytes, _SDMA_DATA_OP, 0,
                                   ctypes.byref(base))
        if not pool or not base.value or not lib.smem_shm_sdma_get_workspace(pool):
            raise RuntimeError("SDMA pool not ready (orch deploy failed?)")
        self._pool = pool
        self._gva = base.value
        self._symm = lib.smem_shm_get_symmetric_size(pool)
        if self._symm < POOL_FLOOR_BYTES:
            raise RuntimeError(
                f"symmetric window {self._symm} below operator pool floor "
                f"{POOL_FLOOR_BYTES}")

        geo = np.zeros((self.world_size, 4), dtype=np.uint64)
        mine = np.array([self._gva, self._symm, 0, self._symm],
                        dtype=np.uint64)
        rc = lib.smem_shm_control_allgather(pool, mine.ctypes.data, 32,
                                            geo.ctypes.data,
                                            32 * self.world_size)
        if rc != 0:
            raise RuntimeError(f"geometry allgather rc={rc}")
        self._peer_gva = int(geo[1 - self.rank][0])
        # 对称窗口强校验：kernel 以 gva + symm*(1-rank) 推算对端池地址，
        # 两端窗口基址必须一致，否则对端地址推导全部失效。
        if (self._peer_gva == 0 or int(geo[1 - self.rank][1]) != self._symm
                or self._peer_gva != self._gva):
            raise RuntimeError(
                f"peer geometry invalid (expected same-base symmetric "
                f"window): {geo.tolist()}")

        # 通道缓冲在建池之后分配（保持“池是本会话首个设备分配”的时序）。
        dev = torch.device(f"npu:{self.device}")
        self._status = torch.zeros(STATUS_BYTES, dtype=torch.uint8, device=dev)
        self._trace = torch.zeros(TRACE_WORDS, dtype=torch.int64, device=dev)

    def barrier(self) -> None:
        """两端 rank 会合点（池控制操作，与 kernel 协议无关）。"""
        rc = self._api.lib.smem_shm_control_barrier(self._pool)
        if rc != 0:
            raise RuntimeError(f"smem_shm_control_barrier rc={rc}")

    def teardown(self) -> None:
        """排空并销毁池（幂等）。缺省此步则进程退出时 smem 库静态析构会
        core dump，因此会话退出前必须调用。"""
        if self._pool is not None:
            self._api.lib.smem_shm_control_barrier(self._pool)
            self._api.lib.smem_shm_destroy(self._pool, 0)
            self._api.lib.smem_shm_uninit(0)
            self._api.lib.smem_uninit()
            self._pool = None
            SmemSession._SESSION = None

    # ---- 发射 -----------------------------------------------------------

    def launch(self, x: torch.Tensor, w_nz: torch.Tensor, rows: int,
               out: torch.Tensor = None) -> torch.Tensor:
        """发射一次融合调度批次（MatMul + AllReduce）。

        x：本 rank 激活 [rows, 2048] fp16 ND 连续；w_nz：本 rank 权重分片
        [2048, 2048] fp16（npu_format_cast(29) 的 NZ 物理布局）；
        out：预分配输出（缺省则分配）。返回写出 AllReduce 结果的 y。
        epoch 由封装层内管（首次发射 1001，逐次 +1），外部无法传错。
        """
        self._check_launch(x, w_nz, rows, out)
        if out is None:
            out = torch.empty(rows, KERNEL_WIDTH, dtype=torch.float16,
                              device=x.device)
        with self._lock:
            self._epoch += 1
            epoch = self._epoch
        self._status.zero_()
        torch_npu.npu_smem_mm_ar_310p3(
            x, w_nz, out, self._gva, self._symm,
            self._status.data_ptr(), self._trace.data_ptr(),
            self.rank, epoch, rows)
        return out

    def _check_launch(self, x, w_nz, rows, out) -> None:
        if rows % KERNEL_ROWS_ALIGN != 0:
            raise ValueError(f"rows={rows} not {KERNEL_ROWS_ALIGN}-aligned")
        if not KERNEL_MIN_ROWS <= rows <= KERNEL_MAX_ROWS:
            raise ValueError(
                f"rows={rows} outside kernel range "
                f"[{KERNEL_MIN_ROWS}, {KERNEL_MAX_ROWS}]")
        if x.dim() != 2 or x.shape[0] != rows or x.shape[1] != KERNEL_WIDTH:
            raise ValueError(
                f"x must be [rows={rows}, {KERNEL_WIDTH}], got {tuple(x.shape)}")
        if x.dtype != torch.float16 or not x.is_contiguous():
            raise ValueError(f"x dtype={x.dtype} contiguous={x.is_contiguous()}")
        if w_nz.dim() != 2 or tuple(w_nz.shape) != (KERNEL_WIDTH, KERNEL_WIDTH):
            raise ValueError(
                f"w_nz must be [{KERNEL_WIDTH}, {KERNEL_WIDTH}], "
                f"got {tuple(w_nz.shape)}")
        if w_nz.dtype != torch.float16:
            raise ValueError(f"w_nz dtype={w_nz.dtype}")
        if out is not None:
            if (out.dim() != 2 or tuple(out.shape) != (rows, KERNEL_WIDTH)
                    or out.dtype != torch.float16 or not out.is_contiguous()):
                raise ValueError(
                    f"out must be [rows={rows}, {KERNEL_WIDTH}] fp16 "
                    f"contiguous, got {tuple(out.shape)} dtype={out.dtype}")

    # ---- 观测接口 -------------------------------------------------------

    @property
    def gva(self) -> int:
        """本端对称池设备虚拟地址。"""
        return self._gva

    @property
    def symm(self) -> int:
        """对称池单侧字节数（对端地址 = gva + symm*(1-rank)）。"""
        return self._symm

    @property
    def epoch(self) -> int:
        """最近一次已发射的调度批次编号。"""
        return self._epoch

    @property
    def status(self) -> torch.Tensor:
        """kernel 错误码通道（uint8，首 8 字节为 64 位错误码，0 = 正常）。"""
        return self._status

    @property
    def trace(self) -> torch.Tensor:
        """kernel 诊断数据通道（int64，每核/每游标一段，见 kernel 注解）。"""
        return self._trace
