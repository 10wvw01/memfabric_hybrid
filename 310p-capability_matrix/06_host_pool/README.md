# 06：Host共享内存与设备映射接口

验证POSIX共享Host内存在两个进程间可见，并作为H2D源使用；分别记录Host注册、映射能力查询和设备指针获取的结果。

## 测试步骤与API

| 阶段 | 操作 | 需要区分的含义 |
|---|---|---|
| 锁页Host基线 | rank0 `aclrtMallocHost`、设备`aclrtMalloc`，H2D/D2H往返并校验 | ACL分配的Host缓冲区可以作拷贝源或目的端 |
| 建立共享Host池 | rank0 `shm_open` → `ftruncate(32MiB)` → `mmap(MAP_SHARED)`并写数据；MPI同步后rank1打开同名对象并映射、校验 | 两进程共享Host物理页，不通过设备IPC共享 |
| 设备拷贝 | rank1以共享页作H2D源，再D2H读回校验；rank0更新共享页后重复 | 验证跨进程更新可见且拷贝取得新数据 |
| 映射能力查询 | `aclrtHostMemMapCapabilities`遍历10种HAC类型 | 调用返回值与cap值分别记录 |
| 注册对照 | V1 `aclrtHostRegister`与V2 `aclrtHostRegisterV2`，分别试MAPPED、PINNED及组合 | 注册返回0不等于取得有效设备地址 |
| 设备地址查询 | 注册后`aclrtHostGetDevicePointer` | 只有实际返回有效映射，才能进一步验证设备直接访问 |
| HAL对照 | `halHostRegister`分别使用flags=0、3 | 独立记录底层返回值，不和ACL返回码混用 |
| 清理 | 成功注册的区域调用Unregister；双方`munmap`和`close`，同步后rank0 `shm_unlink`，各自释放缓冲区 | 共享页、注册状态及设备内存分别管理 |

数据由rank、tag及元素索引组成，rank0更新时更换tag。Host拷贝性能为3次预热、30次16MiB同步拷贝；与共享设备映射的异步D2D不是同一个实验。

10种HAC类型为STARS、AICPU、AIC、AIV、PCIEDMA、RDMA、SDMA、DVPP、UDMA、CCU；本轮cap均为0。保留类型列表便于后续驱动版本逐项比较，但不由该表推断所有Host内存功能都不可用。

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/06_host_pool
A_DEV=2 B_DEV=3 bash run.sh
```

## 2026-11-13测试结果

Ascend310P3，CANN 9.2.1，驱动26.1.1，aarch64。

| 检查项 | 本轮结果 |
|---|---|
| 设备与大小 | 2、3；32MiB共享Host内存，16MiB带宽传输 |
| shm_open/ftruncate/mmap(MAP_SHARED) | 跨进程读写可见，更新后H2D/D2H数据校验通过 |
| aclrtMallocHost | 分配及H2D/D2H校验通过 |
| HostMemMapCapabilities | 10种类型均调用返回0、cap=0（不支持该映射类型） |
| HostRegister V1、V2 MAPPED及MAPPED\|PINNED | 返回207000 |
| HostRegisterV2 PINNED | 对共享mmap及malloc地址均返回0 |
| PINNED后的HostGetDevicePointer | 返回107000，设备指针为空 |
| HAL HostRegister | flags=0返回8，flags=3返回65534，设备指针均为空 |
| 锁页Host带宽 | 3次预热、30次16MiB同步拷贝：H2D 27.01、D2H 24.44 GB/s |
| 总结果 | PASS，两个rank退出0 |

POSIX Host池及Host↔设备拷贝在本轮可用。PINNED注册返回0，但本轮未获得可供设备直接访问的映射地址；因此不再将“部分注册返回0”写成设备映射可用，也不将所有Host注册接口一概写成不可用。已修正程序结尾的误导性提示，不改变测试行为。

## 历史Host拷贝数据

旧README记录了CANN 9.1.0下的四组同设备双进程配置；本轮只运行设备2、3，不覆盖这四组。保留各配置原始测量值，不沿用旧汇总中与配置0数据不一致的13.81/12.61值。

| A_DEV / B_DEV | H2D GB/s | D2H GB/s | 条件 |
|---|---:|---:|---|
| 0 / 0 | 13.93 | 12.57 | 锁页Host，30×16MiB同步拷贝 |
| 2 / 2 | 26.92 | 24.47 | 同上 |
| 4 / 4 | 13.78 | 12.73 | 同上 |
| 6 / 6 | 24.45 | 23.80 | 同上 |

PINNED返回0与取得设备映射是不同结果；注册后的设备指针查询仍失败。这里验证的是Host共享及拷贝能力，没有验证设备直接访问这些Host页或上层SDMA批量算子。
