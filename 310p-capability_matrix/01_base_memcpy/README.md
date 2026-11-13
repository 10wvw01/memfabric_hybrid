# 01：单设备内存分配与拷贝

验证设备内存和锁页Host内存分配，以及H2D、D2H、同设备D2D的同步/异步拷贝。每步写入不同数据后读回校验。

## 测试步骤与API

1. `aclInit` → `aclrtSetDevice(DEV)` → `aclrtCreateStream`。初始化失败直接结束并返回非零。
2. `aclrtMalloc(ACL_MEM_MALLOC_HUGE_FIRST)`分配设备缓冲区；Host侧使用普通`malloc`缓冲区。
3. `aclrtMemcpy(H2D)`写入，再`aclrtMemcpy(D2H)`读回，逐字比较数据。
4. 在两个本地设备缓冲区间分别执行同步`aclrtMemcpy(D2D)`和异步`aclrtMemcpyAsync(D2D)`；异步操作等待`aclrtSynchronizeStream`后读回校验。
5. `aclrtMallocHost`分配锁页Host缓冲区，验证H2D/D2H，与普通Host缓冲区作对照。
6. 正确性通过后测同步/异步、锁页/普通Host的传输带宽；再测4/16/64/256MiB的D2D。
7. `aclrtFreeHost`、`aclrtFree`及Host `free`释放各自缓冲区，销毁stream、重置设备并`aclFinalize`。

每步使用不同tag构造数据，避免上一步残留数据使校验误通过。设备和Host主要缓冲区为128MiB。
同步方式每次调用等待完成；异步连续提交方式在整轮提交后等待一次stream；异步逐次等待方式每提交一次就同步。

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/01_base_memcpy
DEV=2 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。设备及数据规模见下表。

| 检查项 | 本轮结果 |
|---|---|
| 设备与缓冲区 | 设备2；设备缓冲区每块128MiB，另有Host校验缓冲区；D2D尺寸测试为4/16/64/256MiB |
| 内存分配和H2D/D2H | 通过 |
| 同设备同步、异步D2D | 均通过 |
| 无效设备号99 | SetDevice返回107001，程序退出1 |
| 总结果 | PASS，退出0 |


## 拷贝耗时测量

每项3次预热、30次128MiB传输；下表单位GB/s，按传输字节数除以耗时计算。

| 路径 | 本轮GB/s | 说明 |
|---|---:|---:|
| H2D-pin-sync | 28.28 |DMA 直达,CPU 阻塞等待完成 |
| H2D-pin-async | 24.17 |30 次 async 提交 + 1 次 StreamSync,**反而比 sync 慢** |
| D2H-pin-sync | 25.37 | DMA 直达 |
| D2H-pin-async | 21.88 |30 次 async 提交 + 1 次 StreamSync,**反而比 sync 慢** |
| H2D-pg-sync | 19.45 | 驱动内部 staging 中转,两次拷贝 |
| H2D-pg-async | 19.46 |与 sync 接近,受 staging 开销限制 |
| D2H-pg-sync | 18.04 | 驱动内部 staging 中转 |
| D2H-pg-async | 18.02 | 与 sync 接近 |
| D2D-sync | 5.23 | 走 drvMemcpy 路径 |
| D2D-async | 88.26 | 走 SDMA 引擎 |

- pin 表示使用 `aclrtMallocHost()` 分配的 Host 固定内存
- pg 表示使用普通的 `mmap()` 或者 `malloc()` 分配的 pageable 的内存，可能被os换出，在调用 acl API 传输的时候，会先拷贝到内部的staging pinned buffer，再拷贝到 device 侧。

### 本轮D2D尺寸对照
这是单设备内部的 D2D 拷贝。

| 大小MiB | 同步GB/s | 异步连续提交GB/s | 异步逐次等待GB/s |
|---:|---:|---:|---:|
| 4 | 6.99 | 31.59 | 21.02 |
| 16 | 5.21 | 64.77 | 51.30 |
| 64 | 5.25 | 83.97 | 77.26 |
| 256 | 5.27 | 90.53 | 88.61 |

每种大小均为3次预热、30次传输。
