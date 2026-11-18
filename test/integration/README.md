# 集成测试 (Integration Tests)

集成测试目录，用于验证各组件在真实环境下的功能和性能。

## 目录说明

```
test/integration/
├── decrypt/              # 自定义解密库示例
├── etcd_backend/         # Etcd 后端测试工具
├── config_store/         # 配置存储与集群发现测试
├── 310p_sdma_stability/  # 310P SDMA 长稳压测（跨换轮收发 + 全设备同步探针）
└── 310p_mail_loss_repro/ # 310P 批量传输完成、缓冲区复用与 epoch 重启测试
```

## 组件列表

| **名称** | **介绍** | **路径** |
|---------|---------|----------|
| decrypt | 自定义解密库示例，用于 TLS 私钥密码解密 | [decrypt](./decrypt/README.md) |
| etcd_backend | Etcd 后端存储的并发 PrefixGet 性能测试 | [etcd_backend](./etcd_backend/README.md) |
| config_store | 配置存储自动选举、多进程 rendezvous、多集群隔离验证 | [config_store](./config_store/README.md) |
| 310p_sdma_stability | 310P 设备发起 SDMA 长稳压测（换轮零丢失 + serve 死因防回归） | [310p_sdma_stability](./310p_sdma_stability/README.md) |
| 310p_mail_loss_repro | 310P 批量传输完成、缓冲区复用、epoch 重启及错误退出检查 | [310p_mail_loss_repro](./310p_mail_loss_repro/README.md) |
| bm_pool_fallback | 310P BM 同卡与跨卡设备内存回退、四rank混合分配及主机DRAM回归 | [bm_pool_fallback](./bm_pool_fallback/README.md) |
| 310p_aicpu_deploy | AICPU 自动部署、失败后重新启动与设备文件清理 | [310p_aicpu_deploy](./310p_aicpu_deploy/README.md) |
| 310p_package_install | 安装包及 wheel 的 Python SHMEM 通信验证 | [310p_package_install](./310p_package_install/README.md) |
| 310p_kernel_get | kernel 内生产数据、ready 通知、get 校验与跨轮缓冲复用 | [310p_kernel_get](./310p_kernel_get/README.md) |

## 运行步骤

1. 在上表选择测试，阅读对应 README 的设备、拓扑及依赖要求。
2. 加载 CANN 等运行环境，并按对应 README 构建所需主库、安装包或测试程序。
3. 选择空闲设备，按对应 README 指定的环境变量和命令运行，日志放入独立目录。
4. 检查各项数据及状态校验、进程退出码和日志；超时或任一检查失败均需排查。
