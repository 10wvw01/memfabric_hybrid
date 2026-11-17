# 310P AICPU 部署诊断工具

SHMEM SDMA 建池时由产品库自动部署 AICPU kernel。构建产物使用固定名称：
`libmf_sdma_orch.so`、`libmf_sdma_orch.json`、`libmf_sdma_orch_launch.json`。
三份文件需要位于同一目录，可通过 `MF_SDMA_ORCH_JSON` 指定 launch JSON。

`smem_shm_init` 初始化配置存储和设备；`smem_shm_create` 选择 SDMA 后触发部署。
每次首次 Acquire 生成独立的临时加载名称，先确认设备目标文件缺失，再通过 CUST
迁移 SO，复制到 CP1 搜索目录并检查入口名和文件大小，最后加载并启动。
临时加载名称由运行时生成，使用者无需维护版本号。已有部署文件不会被当作本次
部署成功的依据；CP1 加载缓存按本次独立名称区分。

最后一个引用释放时，库先停止 kernel，再删除本次创建的设备文件。
其他进程及历史部署文件保持原样。异常退出可能留下临时设备文件，后续进程使用
新的名称完成部署。设备通道失败或部署失败会返回错误。

部署日志包含 `automatic deploy begin`、`kernel deployed to CP1 search path`、
`automatic deploy file removed` 和具体设备路径，可用于核对缺失、复制、清理过程。

## 手动诊断

`make ASCEND_HOME_PATH=<CANN目录>` 构建 deployer 和 mini_kfc。
mini_kfc 可查询指定设备上的文件；deployer 用于单独排查 CUST 迁移通道。
两个工具仅用于开发诊断，保持在产品安装包外。

CUST 迁移 JSON 的基名与 SO 配对。`BinaryLoadFromFile(cpuMode=1)` 配合
`BinaryGetFunction` 触发迁移。当前进程通过以下设备侧链接读取 SO：

`/home/CustAiCpuUser/cust_aicpu_<dieId>_<vfId>_<hostPid>/libmf_sdma_orch.so`

其中 dieId 为卡内 die 编号，hostPid 为执行迁移的主机进程号；该链接指向 CANN
管理的迁移文件。查询和复制须使用当前 die、当前进程的链接，并保持迁移进程及
句柄存活。使用 deployer 时，hostPid 可从后台进程的 $! 获取。
mini_kfc 可用 grep -q HybmSdmaOrchEpoch 检查该链接指向的文件内容。
随后通过 KFC 在设备 OS 中复制到 `/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/`。

## 回退与审计

回退代码提交并重新构建、部署主库与 AICPU 三份产物，可恢复原有部署流程。
修复仅删除本次生成并完成归属标记的设备文件。排查时保存部署日志及目标文件路径；
检查首次自动部署、后续重新部署、部署失败返回和释放后的文件状态。

CUST 迁移源通过当前 die、当前进程的专属链接读取，迁移前保留 CANN 已建立引用的文件。
memfabric 创建的 CP1 目标文件在失败或正常释放时清理；CANN 文件的回收时机需按运行环境确认。
