# 集成测试 (Integration Tests)

集成测试目录，用于验证各组件在真实环境下的功能和性能。

## 目录说明

```
test/integration/
├── decrypt/              # 自定义解密库示例
├── etcd_backend/         # Etcd 后端测试工具
├── config_store/         # 配置存储与集群发现测试
├── 310p_sdma_stability/  # 310P SDMA 长稳压测（跨换轮收发 + 全设备同步探针）
└── 310p_hccl_allreduce/  # 310P 卡内双芯 HCCL AllReduce 验证（正确性 + KFC 通道取证）
```

## 组件列表

| **名称** | **介绍** | **路径** |
|---------|---------|----------|
| decrypt | 自定义解密库示例，用于 TLS 私钥密码解密 | [decrypt](./decrypt/README.md) |
| etcd_backend | Etcd 后端存储的并发 PrefixGet 性能测试 | [etcd_backend](./etcd_backend/README.md) |
| config_store | 配置存储自动选举、多进程 rendezvous、多集群隔离验证 | [config_store](./config_store/README.md) |
| 310p_sdma_stability | 310P 设备发起 SDMA 长稳压测（换轮零丢失 + serve 死因防回归） | [310p_sdma_stability](./310p_sdma_stability/README.md) |
| 310p_hccl_allreduce | 310P 卡内双芯 HCCL AllReduce 单次验证（数据正确性 + HcclKernelEntrance/KFCKernel 执行链取证） | [310p_hccl_allreduce](./310p_hccl_allreduce/README.md) |
