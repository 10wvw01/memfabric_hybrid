## Purpose

定义310P设备池、TRANS VMM缓冲区和SHMEM元数据区的HAL内存生命周期。目录名保留既有引用。

## Requirements

### Requirement: 310P内存后端与设备编号

310P HBM池 SHALL 由HybmVmmBasedSegment管理，TRANS SHALL 保持用户内存注册接口。
两者及固定地址元数据区 SHALL 使用HAL预留、物理分配、映射、访问权限及释放。
HAL参数 SHALL 使用由程序设备编号转换得到的驱动逻辑编号。物理编号用于拓扑判断和跨rank交换。
ACL继续负责本地设备上下文、设备查询、stream、数据拷贝和设备P2P控制。

#### Scenario: 可见设备重编号

- **WHEN** 当前进程的设备0对应物理设备2
- **THEN** HAL使用转换后的驱动逻辑编号；同卡判断使用物理编号

### Requirement: 物理内存类型及回退

310P SHALL 使用MEM_HUGE_PAGE_TYPE与MEM_P2P_DDR_TYPE。
仅当HalMemCreate返回6时 SHALL 以MEM_DDR_TYPE重试一次。
其他错误 SHALL 记录设备编号、大小及返回码并返回失败。

#### Scenario: P2P分配不足

- **WHEN** P2P_DDR分配返回6
- **THEN** 使用DDR重试；重试失败时释放已预留的地址窗口

### Requirement: 共享句柄与导入上下文

310P SHALL 使用64位NONE共享句柄，并设置既有跨进程共享属性。
HBM池保留HostSdmaExportInfo布局；TRANS保留UserHbmExportSliceInfo布局及公共注册接口。
HAL同卡与跨卡导入 SHALL 显式传入本端驱动逻辑编号，并保持当前线程的本地设备上下文。

#### Scenario: 同卡导入

- **WHEN** 两个rank位于同卡不同die并导入HAL共享句柄
- **THEN** 导入、映射及权限设置均在本端完成，数据比较通过

### Requirement: 地址窗口与元数据

HBM池 SHALL 按rankCnt乘maxSize预留窗口，并以rank槽位安放本地及导入分片。
元数据区 SHALL 精确预留HYBM_DEVICE_META_ADDR并授予本端设备访问权限。
本端分配只设置本端访问权限，对端在导入映射后设置自己的访问权限；设置失败时 SHALL 返回失败并释放映射。
TRANS可以使用独立的本地映射地址，通过交换的地址与地址转换表访问对端。
VMM映射范围按2MiB对齐，注册和拷贝仍受实际注册大小约束。

### Requirement: 注册、导出与释放同步

VMM注册表 SHALL 在同一把锁内查询、导出或释放物理句柄。
未注册地址与已注册地址的HAL导出失败 SHALL 分别返回对应状态。
注册范围超过实际分配大小时 SHALL 返回失败。
普通非VMM用户内存的IPC兼容分支保持其现有接口。

### Requirement: 失败清理与重试

TRANS缓冲区和元数据区的释放 SHALL 按解除映射、释放物理句柄、释放地址窗口的顺序进行。
这两类资源的每一步释放成功后 SHALL 更新资源状态，后续重试从尚未完成的步骤继续。
HAL导入失败 SHALL 释放地址预留；映射或权限设置失败 SHALL 释放已取得的导入资源。
元数据释放失败时 SHALL 保留清理状态并允许重试，清理完成前拒绝重新初始化。

### Requirement: 其他型号兼容

其他SOC的内存后端及P2P释放失败处理保持原行为。
ACL VMM兼容符号仍可选加载；310P HAL内存流程不调用这些物理内存接口。
