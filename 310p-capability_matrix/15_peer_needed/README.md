# 15_peer_needed — 跨卡 IPC 是否需要 enable peer(干净对照)

Atlas 300I Duo (310P3) / CANN 9.1.0 / aarch64,MPI 双进程:
rank0=exporter(dev A,`A_DEV` 默认 0),rank1=importer(dev B,`B_DEV` 默认 2)。

## 目的
干净回答 **"跨卡 IPC import 是否需要 enable peer"**。

本测试每次运行只测一个 MODE,进程独立,互不干扰。

## 五个 MODE(环境变量 `MODE`,每次独立 mpirun)

| MODE | importer 行为 | 结果 |
|---|---|---|
| `DEFAULT` | `ImportByKey(key, DEFAULT)`,不做任何 enable | 507899 |
| `ENABLE` | rank1 单向 `aclrtDeviceEnablePeerAccess(A_DEV)`,再 `ImportByKey(key, DEFAULT)` | 仍 507899 |
| `ENABLE_R0` | rank0 单向 `aclrtDeviceEnablePeerAccess(B_DEV)`,再 DEFAULT import | 仍 507899 |
| `ENABLE_BOTH` | rank0+r1 双向 `aclrtDeviceEnablePeerAccess`,再 `ImportByKey(key, DEFAULT)` | import 0 + D2D OK |
| `PEER` | `ImportByKey(key, ENABLE_PEER_ACCESS)` | import 0 + D2D OK |

import 成功后追加 `async D2D 读 + D2H 逐字校验`,验证数据通路真实可用。

## 运行
```
./run.sh                  # 依次跑 DEFAULT/ENABLE/ENABLE_R0/ENABLE_BOTH/PEER
MODE=DEFAULT A_DEV=0 B_DEV=2 mpirun --allow-run-as-root -np 2 ./test  # 单跑一个 MODE
```

## 结论(2026-08-27 实测验证)

**跨卡 IPC import 的必要条件是"双向 P2P 均已使能";PEER flag 并非唯一方式——手动双向 `aclrtDeviceEnablePeerAccess` + DEFAULT flag 同样可行(ENABLE_BOTH 已验证)。**

五个 MODE 结果:
- **DEFAULT → 507899**:无 P2P,跨卡必然失败。
- **ENABLE → 507899**:rank1 单向(dev2→dev0)不够。
- **ENABLE_R0 → 507899**:rank0 单向(dev0→dev2)也不够——证明不是"缺某个特定方向",是**双向都要**。
- **ENABLE_BOTH → import OK + D2D OK**:两端各调一次 `EnablePeerAccess`,DEFAULT flag 即可跨卡成功。
- **PEER → import OK + D2D OK**:`ENABLE_PEER_ACCESS` flag 内部自动做双向 P2P。

### 源码级解释(为什么单向不够)

Runtime 侧(`api_impl_standard_soc.cc:687-693`):PEER flag 分支调 `EnableP2PWithOtherDevice`
(`raw_device.cc:2198`)= 两次 `halDeviceEnableP2P`(双向);之后 open 走 v2 devmm 路径
(本机 `libascend_hal.so` 只导出 `halShmemOpenHandleByDevId`,无 v3 的 V2 接口):
`halShmemOpenHandleByDevId` → alloc VA + `DEVMM_SVM_PREFETCH` ioctl →
`devmm_ipc_page_table_create_process` → 设备侧 daemon 建 P2P 页表。

关键在内核 devdrv(`devdrv_ctrl.c:4588`):

```c
/* First configuration, and peer already configuration */
if ((p2p_attr->ref == 0) && (p2p_peer_attr->ref > 0)) {
    /* Both directions are configured to take effect */
    ret = devdrv_p2p_cfg_enable(index_id, peer_index_id, p2p_type);
```

- 每次 `aclrtDeviceEnablePeerAccess` 只给一个方向计 ref;**硬件配置仅在第二个方向首次使能时才下发**
- `devdrv_p2p_cfg_enable` 一次配好双向:IOMMU 映射 + PCIe TX ATU 窗口 + P2P 消息通道(`devdrv_ctrl.c:4350`)
- 单向 enable → ATU 窗口未建 → 对端 HBM 在本设备地址空间不可见 → 设备侧建 P2P 页表失败 → 507899
- `devdrv_is_p2p_enabled` 同样要求 `ref>0 && peer_ref>0`(`devdrv_ctrl.c:4809`)
- 同卡 HCCS 免配置(`devdrv_init_p2p_io_mem` 对 HCCS 直接 return 0)→ 同卡 IPC 无需任何 enable

**实用结论**:
1. 推荐直接用 `ENABLE_PEER_ACCESS` flag,自动处理双向 P2P。
2. 若用 DEFAULT flag,必须两端各调一次 `aclrtDeviceEnablePeerAccess`(或单进程 SetDevice 切换调两次,见官方文档示例)建立双向 P2P。
