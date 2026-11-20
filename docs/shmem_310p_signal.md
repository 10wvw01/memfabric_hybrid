# 310P SHMEM通知API

包含 `smem_shm_aicore_base_sdma.h`。参数沿用现有SDMA API：源在前、目标在后。`_at` 接收 `smem_shm_sdma_ctx(gva)` 返回的上下文，其余远端操作也提供首参数为gva的便捷封装。

| API | 成功的含义 |
|---|---|
| `smem_shm_sdma_put_signal_nbi_at(ctx, src, dst, bytes, signal, value, op)` | 数据操作和64位SET通知已入队 |
| `smem_shm_sdma_put_signal_at(...)` | 当前block已提交操作完成，包括本次数据和通知 |
| `smem_shm_sdma_signal_set_nbi_at(ctx, signal, value)` | 独立SET通知已入队 |
| `smem_shm_sdma_signal_set_at(ctx, signal, value)` | 当前block已提交操作完成，包括通知 |
| `smem_shm_signal_test(localSignal, cmp, value)` | 比较成立；不成立立即返回false |
| `smem_shm_signal_wait_until(localSignal, cmp, value, budget)` | 预算内比较成立；参数无效或预算耗尽返回false |

op默认COPY，支持既有ADD_FP32、ADD_FP16数据操作。通知为64位SET，单个通知槽由一个发送者写入。signal按8字节对齐，位于目标peer的有效共享段。每个通知独占64字节缓存行，避免接收方其他写入覆盖远端更新。

比较枚举为 `SMEM_SHM_CMP_EQ/NE/GT/GE/LT/LE`，按无符号64位比较；wait的budget以检测次数计，默认沿用SDMA预算。test/wait维护通知缓存行；数据缓存维护由调用方负责。

## 完成和复用

AICPU在同一SDMA SQ中按“数据操作 → 远端通知 → 本地完成戳”提交。接收方观察到本轮通知时，本次数据操作已完成，随后按需维护数据缓存并消费。

NBI成功表示请求入队，源数据保持有效至quiet成功。同步和quiet仅等待调用block的队列。接收缓冲区再次被覆盖前，需要另行确认接收方消费完成。

零长度put_signal只发布通知，忽略src/dst；普通零长度put/get仍直接成功。SET可使用任意64位值，连续轮次使用可区分的序号，并协调通知槽复用。

```cpp
smem_shm_sdma_put_signal_nbi_at(ctx, source, remoteData, bytes, remoteReady, seq);
// 可执行其他独立工作，源数据保持有效。
smem_shm_sdma_quiet_at(ctx);

// 接收方：成功后维护数据缓存并读取。
smem_shm_signal_wait_until(localReady, SMEM_SHM_CMP_EQ, seq);
```

队列按 `(peer, GetBlockIdx())` 分配，每队列单生产者。8个block使用各自队列；不同stream的kernel使用相同block编号时，需要调用方协调。当前不提供多写者原子signal ADD。

Host库、AICPU产物和使用新增设备API的应用需配套构建；运行时协议标识用于检查Host与AICPU内核是否匹配。

功能测试：`test/integration/310p_shmem_signal`。统一MF/HCCL基准：`benchmark/shmem_310p`。
