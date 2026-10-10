# Memory Pool Examples Design Overview

本目录仅包含 内存池（Memory Pool）能力的样例设计。

## 目录分层
- 01_basic：单机单卡最小闭环与基础池类型（DRAM/HBM）
- 02_scale_out：单机多卡与多机多卡扩展
- 03_optimization：内存注册、批量拷贝与 device_sdma 协议性能对比
- 04_features：特性开关类最小样例（先放 unified address space）
- 05_observability：观测体系设计（Prometheus + Grafana）

## 推荐学习顺序
1. 01_basic/01_single_device_dram_pool
2. 01_basic/02_single_device_dram_configurable_pool
3. 01_basic/03_single_device_hbm_pool
4. 01_basic/04_no_xpu_host_rdma_dram_pool
5. 01_basic/05_no_xpu_host_urma_dram_pool
6. 01_basic/06_single_card_external_stream
7. 01_basic/07_310p_die_pair_pool（310P 专属：单卡双 die 双池，需 Atlas 300I Duo）
8. 01_basic/08_310p_aicore_aicpu_sdma（310P 专属：设备发起 SDMA 协同，需 Atlas 300I Duo，上手见[310P 使用指南](../../docs/310p/使用指南.md)）
9. 02_scale_out/01_single_node_multi_device_dram
10. 02_scale_out/02_multi_node_multi_device_dram
11. 03_optimization/01_copy_data_batch
12. 03_optimization/02_register
13. 03_optimization/03_device_sdma
14. 04_features/01_enable_unified_address_space
15. 05_observability/01_prometheus_grafana
16. 05_observability/02_opentelemetry
17. 05_observability/03_dashboards

## 统一约束
- 所有样例仅使用内存池接口。
- 每个样例 README 都包含：场景、目标、使用能力、规模建议、必要条件、验收标准。
