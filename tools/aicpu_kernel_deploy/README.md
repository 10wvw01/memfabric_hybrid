# tools/aicpu_kernel_deploy — 310P CP1 部署链工具（手动/诊断用）

> **定位**：把自定义 AICPU kernel `.so` 放进设备 CP1 搜索路径
> （`/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`）的手动部署与诊断工具。
> **产品库初始化已内置自动部署**（`src/hybm/csrc/entity/hybm_sdma_orchestrator.cpp`，
> 首次加载失败时自动走迁移+就位+校验），常规使用**无需本工具**；
> 本工具用于预部署、部署链排查、以及非产品 kernel 的手动落盘。

`deployer` 的入口函数名经参数传入（默认 `HybmSdmaOrchEpoch`，与产品 json 一致）。

## 构建

```bash
make ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0   # 产物: deployer mini_kfc
```

## 机制（为什么需要"迁移+复制"两步）

- `deployer`：`aclrtBinaryLoadFromFile(cpuMode=1)` 本身不落盘，`aclrtBinaryGetFunction`
  才触发 CUST 迁移通道把 `.so` 写到设备 `/home/CustAiCpuUser/lib/<so>.<devId>.<vfId>.<pid>`；
  **迁移进程退出会触发清理链延迟删除设备侧文件**（故需保活）。
- **json 基名配对（实测约束）**：CUST 加载器要求 json 文件基名与 `.so` 同名
  （异名 `load ret=107000`，即使内容与 `.so` 逐字节一致）；部署 json 须命名为
  `<soname>.json`（如 `libmf_wd_probe.json` 配 `libmf_wd_probe.so`）。
- CUST 落盘路径不在 CP1 搜索路径内，须由 `mini_kfc` 在设备 OS 内复制到
  `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/` 并 `chmod 644`。
- `mini_kfc`：KFC 通道（json 声明 `KFCKernel` + `functionName: "system"` + 不存在的
  `kernelSo` → CP1 内 handler dlopen 失败回退 `dlsym` 命中 libc `system()`），
  host 一条 launch 即在设备 OS 内执行 shell 命令；结果看 stdout 的 `sync=`（0=成功）。

## 手动部署链（kernel 三件套由 `src/hybm/ops` 的 `mf_sdma_orch_kernel` 目标产出）

前置：`libmf_sdma_orch.so`（kernel 本体）+ `libmf_sdma_orch.json`（CUST 迁移 json）+
`libmf_sdma_orch_v4.json`（launch json），三者同目录（主构建产物
`<build>/src/hybm/ops/hybm_kernel/`、独立构建产物 `<build-kernel>/hybm_kernel/`，
或安装前缀 `<prefix>/hybm/aicpu_kernel/`）。

```bash
cd tools/aicpu_kernel_deploy && make
KERNEL_DIR=<上述三件套所在目录>
./mini_kfc 0 "rm -f /usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch*"   # 清旧版
(./deployer 0 $KERNEL_DIR/libmf_sdma_orch.json 300 > /tmp/deploy.log 2>&1 &)            # 迁移+保活
sleep 22                                                                                  # 迁移完成裕量
./mini_kfc 0 "cat /home/CustAiCpuUser/lib/libmf_sdma_orch.so.* > \
/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_v4.so && chmod 644 \
/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_v4.so"                  # 复制+授权
./mini_kfc 0 "grep -q HybmSdmaOrchBatch \
/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_v4.so"                  # 验证门(sync=0 即过)
```

## 运维纪律

- **保活语义**：`deployer` 退出后清理链会（延迟）删除设备侧文件——300 秒保活期内须完成
  验证门与正式运行；到期后重跑本链即可。
- **版本切换**：设备侧文件名带版本（`libmf_sdma_orch_v4.so`），换版本=新文件名，
  CP1 dlopen 缓存自然失效（KVER 递增即缓存破坏）。
- **同卡双 die 共享设备 OS 文件系统**：dev0 上部署对 dev1 同样生效。

## 安全告示

`mini_kfc` 在设备 OS 内执行任意 shell 命令，**仅限开发/诊断环境使用，禁止随产品
运行时分发**。产品库内置的自动部署只执行固定格式的复制/校验命令串（路径与文件名
均为受控常量），见 `hybm_sdma_orchestrator.cpp` 的 `KfcExec`。
