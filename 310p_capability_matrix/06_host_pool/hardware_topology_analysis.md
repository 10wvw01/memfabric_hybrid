# Atlas 300I Duo 硬件拓扑与 PCIe 带宽分析

## 1. 机器配置概览

**服务器配置：**
- CPU: Kunpeng 920 (4 NUMA 节点, 192 核)
- 加速卡: 4 × Atlas 300I Duo (310P3)
- 每张卡: 2 die (NPU), 卡内通过 HCCS 互联
- 系统: Ubuntu 22.04, CANN 9.1.0

**NPU 设备映射：**
| 卡号 | BDF | NPU ID | NUMA | 设备类型 |
|------|-----|--------|------|----------|
| 卡0 | 18:00.0 | 0, 1 | 1 | Atlas 300I Duo |
| 卡1 | 41:00.0 | 2, 3 | 1 | Atlas 300I Duo |
| 卡2 | 96:00.0 | 4, 5 | 3 | Atlas 300I Duo |
| 卡3 | AB:00.0 | 6, 7 | 3 | Atlas 300I Duo |

## 2. PCIe 拓扑分析

### 2.1 完整拓扑树
```bash
lspci -t
```

```
-[0000:16]-+-00.0-[17]----00.0   ← RAID 控制器
           \-04.0-[18]----00.0   ← 卡0 (NPU 0,1)
-[0000:40]---00.0-[41]----00.0   ← 卡1 (NPU 2,3)
-[0000:95]---00.0-[96]----00.0   ← 卡2 (NPU 4,5)
-[0000:aa]---00.0-[ab]----00.0   ← 卡3 (NPU 6,7)
```

### 2.2 Root Port 链路能力对比

**分析方法：**
```bash
# 查看每个 Root Port 的链路能力和状态
for port in 16:04.0 40:00.0 95:00.0 aa:00.0; do
  echo "=== $port ==="
  lspci -s $port -vvv 2>&1 | grep -E "LnkCap:|LnkSta:"
done
```

**结果：**
| Root Port | 对应卡 | LnkCap (能力) | LnkSta (状态) | 实际带宽 |
|-----------|--------|---------------|---------------|----------|
| 16:04.0 | 卡0 | PCIe 4.0 **x8** | 16GT/s x8 (ok) | x8 |
| 40:00.0 | 卡1 | PCIe 5.0 **x16** | 16GT/s x16 (ok) | x16 |
| 95:00.0 | 卡2 | PCIe 4.0 **x8** | 16GT/s x8 (ok) | x8 |
| aa:00.0 | 卡3 | PCIe 5.0 **x16** | 16GT/s x16 (ok) | x16 |

**关键发现：**
1. 卡0/卡2 的 Root Port **物理上只支持 x8** (LnkCap: Width x8)
2. 卡1/卡3 的 Root Port 支持 **PCIe 5.0 x16**，但 Atlas 300I Duo 只支持 PCIe 4.0，降速到 16GT/s
3. 带宽瓶颈在主板插槽设计，不是加速卡本身

### 2.3 连接架构图

```
CPU Socket 0 (NUMA 0,1)
├─ PCIe Root Port 16:04.0 (PCIe 4.0 x8) ──→ 卡0 (NPU 0,1)
│                                             ├─ die0 ←─ HCCS (24Gbps×8lane) ─→ die1
│                                             └─ 8GB HBM2e
├─ PCIe Root Port 40:00.0 (PCIe 5.0 x16, 降速到 4.0) ──→ 卡1 (NPU 2,3)
│                                                           ├─ die0 ←─ HCCS ─→ die1
│                                                           └─ 8GB HBM2e
└─ ...

CPU Socket 1 (NUMA 2,3)
├─ PCIe Root Port 95:00.0 (PCIe 4.0 x8) ──→ 卡2 (NPU 4,5)
│                                             ├─ die0 ←─ HCCS ─→ die1
│                                             └─ 8GB HBM2e
├─ PCIe Root Port aa:00.0 (PCIe 5.0 x16, 降速到 4.0) ──→ 卡3 (NPU 6,7)
│                                                           ├─ die0 ←─ HCCS ─→ die1
│                                                           └─ 8GB HBM2e
└─ ...

卡内互联:
  die0 ←─ HCCS (24Gbps × 8 lanes = 24GB/s) ─→ die1
```

## 3. H2D/D2H 带宽测试

### 3.1 测试方法

**测试代码：** `06_host_pool/test.c`

**测试参数：**
- 数据大小: 16MB × 30 次
- 预热: 3 次
- 内存类型: host pinned memory (aclrtMallocHost)
- 传输模式: 同步 aclrtMemcpy

**运行命令：**
```bash
# 卡0 (PCIe 4.0 x8)
A_DEV=0 B_DEV=0 ./run.sh

# 卡1 (PCIe 4.0 x16)
A_DEV=2 B_DEV=2 ./run.sh

# 卡2 (PCIe 4.0 x8)
A_DEV=4 B_DEV=4 ./run.sh

# 卡3 (PCIe 4.0 x16)
A_DEV=6 B_DEV=6 ./run.sh
```

### 3.2 测试结果

| 卡 | PCIe 链路 | H2D 带宽 | D2H 带宽 | 效率 |
|----|-----------|----------|----------|------|
| 卡0 (NPU 0,1) | PCIe 4.0 x8 | **13.81 GB/s** | **12.61 GB/s** | ~88% |
| 卡1 (NPU 2,3) | PCIe 4.0 x16 | **26.92 GB/s** | **24.47 GB/s** | ~85% |
| 卡2 (NPU 4,5) | PCIe 4.0 x8 | **13.78 GB/s** | **12.73 GB/s** | ~88% |
| 卡3 (NPU 6,7) | PCIe 4.0 x16 | **24.45 GB/s** | **23.80 GB/s** | ~77% |

### 3.3 理论带宽对比

**PCIe 带宽计算公式：**
```
带宽 = 速率 × lane数 × 编码效率
PCIe 4.0: 16 GT/s, 128b/130b 编码 (效率 98.5%)
PCIe 5.0: 32 GT/s, 128b/130b 编码 (效率 98.5%)
```

**理论值：**
| PCIe 配置 | 理论带宽 | 实测带宽 | 效率 |
|-----------|----------|----------|------|
| PCIe 4.0 x8 | 15.75 GB/s | ~13.8 GB/s | ~88% |
| PCIe 4.0 x16 | 31.51 GB/s | ~25-27 GB/s | ~80-85% |

**效率损失原因：**
- PCIe 协议开销 (TLP 包头、DLLP 流控)
- DMA 引擎性能限制
- Host 内存带宽竞争
- API 调用开销 (同步模式)

### 3.4 关键结论

1. **带宽完全由 PCIe lane 数量决定**
   - x16 卡带宽是 x8 卡的 ~1.9 倍
   - 与理论值吻合

2. **H2D 和 D2H 带宽差异**
   - H2D: CPU 写, 设备读
   - D2H: 设备写, CPU 读
   - 两个方向的 DMA 引擎、缓存行为不同

3. **主板插槽设计限制**
   - 卡0/卡2 的插槽只提供 x8 lane
   - 卡1/卡3 的插槽提供 x16 lane
   - 这是服务器主板的典型设计（某些插槽与其他设备共享 lane）

## 4. 分析方法总结

### 4.1 PCIe 拓扑分析命令

```bash
# 1. 查看完整 PCIe 拓扑树
lspci -t

# 2. 查看设备详细信息
lspci -s <BDF> -vvv

# 3. 查看链路能力和状态
lspci -s <BDF> -vvv | grep -E "LnkCap:|LnkSta:"

# 4. 批量查看所有 Root Port
for dev in $(lspci | grep "Root Port" | awk '{print $1}'); do
  echo "=== $dev ==="
  lspci -s $dev -vvv 2>&1 | grep -E "LnkCap:|LnkSta:"
done
```

### 4.2 NPU 设备查询命令

```bash
# 查看 NPU 设备信息
npu-smi info

# 查看 NPU 拓扑
npu-smi info -t topo

# 查看 HCCS 连接
npu-smi info -t hccs -i <card_id> -c <chip_id>
```

### 4.3 带宽测试命令

```bash
# 编译测试程序
cd 06_host_pool
make

# 运行测试（指定设备）
A_DEV=<device_id> B_DEV=<device_id> ./run.sh

# 提取带宽数据
./run.sh 2>&1 | grep "bw"
```

### 4.4 分析流程

1. **确定设备 BDF**：`npu-smi info` 查看 NPU 对应的 BDF
2. **找到 Root Port**：`lspci -t` 从拓扑树找到上级 Root Port
3. **查看链路能力**：`lspci -s <BDF> -vvv` 查看 LnkCap 和 LnkSta
4. **对比分析**：对比不同卡的 Root Port 配置，找出差异
5. **带宽测试**：运行测试程序验证实际带宽
6. **得出结论**：确认瓶颈在 Root Port（主板插槽）还是设备本身

## 5. 建议

### 5.1 性能优化

- **使用 x16 插槽**：卡1 和卡3 提供更高带宽（~25-27 GB/s vs ~14 GB/s）
- **NUMA 亲和性**：将进程绑定到设备对应的 NUMA 节点
  - 卡0/卡1 → NUMA 1 (CPU 48-95)
  - 卡2/卡3 → NUMA 3 (CPU 144-191)

### 5.2 主板配置检查

如果需要所有卡都达到 x16 带宽：
1. 查阅服务器主板手册，确认 PCIe 插槽配置
2. 检查 BIOS 设置，确认 PCIe 插槽配置正确
3. 某些主板需要手动配置 PCIe 插槽的 lane 分配

## 6. 参考文档

- Atlas 300I Duo 产品规格
- Kunpeng 920 PCIe 控制器规格
- PCI Express Base Specification 4.0/5.0
- CANN 9.1.0 开发文档
