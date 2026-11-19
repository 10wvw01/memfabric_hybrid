# 310P AICPU 部署诊断工具

310P SHMEM 初始化时由产品库自动部署 AICPU kernel。构建产物使用固定名称：
`libmf_sdma_orch.so`、`libmf_sdma_orch.json`、`libmf_sdma_orch_launch.json`。
三份文件需要位于同一目录，可通过 `MF_SDMA_ORCH_DIR` 指定产物目录或其上级目录进行递归查找。

`smem_shm_init` 初始化配置存储和设备，并持有部署资源引用。
初始化首次 Acquire 生成独立的临时加载名称，先确认设备目标文件缺失，再通过 CUST
迁移 SO，复制到 CP1 搜索目录并检查入口名和文件大小，最后加载。
`smem_shm_create` 选择 SDMA 后绑定内存池并启动搬运 kernel。
临时加载名称由运行时生成，使用者无需维护版本号。已有部署文件不会被当作本次
部署成功的依据；CP1 加载缓存按本次独立名称区分。

最终 `hybm_uninit` 释放初始化持有的引用。最后一个引用释放时，库先停止 kernel，再删除本次创建的设备文件。
其他进程及历史部署文件保持原样。异常退出可能留下临时设备文件，后续进程使用
新的名称完成部署。设备通道失败或部署失败会返回错误。

部署日志包含 `automatic deploy begin`、`kernel deployed to CP1 search path`、
`automatic deploy file removed` 和具体设备路径，可用于核对缺失、复制、清理过程。

## 手动诊断

deployer 用于单独测试 CUST 迁移通道，mini_kfc 用于执行设备侧查询和复制。
两个工具仅用于开发诊断，保持在产品安装包外。

### 构建与通道检查

在仓库根目录执行，`ASCEND_HOME_PATH` 指向本机 CANN 安装目录：

```bash
make -C tools/aicpu_kernel_deploy ASCEND_HOME_PATH="$ASCEND_HOME_PATH"
TOOLS_DIR="$(pwd)/tools/aicpu_kernel_deploy"
DEVICE_ID=0
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" "echo mf_sdma_channel_ok"
```

通道检查成功时输出 `launch=0 sync=0`。失败时先排查设备状态、CANN 环境和 KFC 通道，
再继续迁移测试。

### 迁移、检查与复制

`ARTIFACT_DIR` 指向包含三份产物的实际目录；deployer 直接接收 JSON 文件路径。
CUST 迁移 JSON 的基名与 SO 配对，`BinaryLoadFromFile(cpuMode=1)` 配合
`BinaryGetFunction` 触发迁移。

```bash
ARTIFACT_DIR=/path/to/aicpu_kernel
"$TOOLS_DIR/deployer" "$DEVICE_ID" "$ARTIFACT_DIR/libmf_sdma_orch.json" 600 >deployer.log 2>&1 &
DEPLOY_PID=$!
cat deployer.log
```

等待日志出现 `getfunc ret=0` 后继续。日志尚未输出时可再次查看；进程提前退出时先检查错误。
保活时间内，当前进程通过以下设备侧链接读取 SO：

`/home/CustAiCpuUser/cust_aicpu_<dieId>_<vfId>_<hostPid>/libmf_sdma_orch.so`

其中 dieId 为卡内 die 编号，hostPid 为执行迁移的主机进程号，vfId 由运行环境决定。
该链接指向 CANN 管理的迁移文件。查询和复制时保持 deployer 进程存活，
下面的 `DIE_ID` 按目标设备的实际映射填写：

```bash
DIE_ID=0
SOURCE_GLOB="/home/CustAiCpuUser/cust_aicpu_${DIE_ID}_*_${DEPLOY_PID}/libmf_sdma_orch.so"
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" "ls -l $SOURCE_GLOB"
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" "grep -q HybmSdmaOrchEpoch $SOURCE_GLOB"
```

复制时使用本次手动测试的独立名称，复制命令先检查源文件恰好匹配一个、目标文件尚不存在：

```bash
DEVICE_SO="/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_manual_${DEPLOY_PID}.so"
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" \
    "set -- $SOURCE_GLOB; test \$# -eq 1 && test -f \"\$1\" && test ! -e '$DEVICE_SO' && cp \"\$1\" '$DEVICE_SO'"
HOST_SO_SIZE=$(wc -c < "$ARTIFACT_DIR/libmf_sdma_orch.so")
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" \
    "grep -q HybmSdmaOrchEpoch '$DEVICE_SO' && ls -l '$DEVICE_SO' | awk '{print \$5}' | grep -qx '$HOST_SO_SIZE'"
```

确认各命令均返回 `launch=0 sync=0`，最后一条设备检查同时验证入口字符串及主机与设备 SO 字节数。
设备侧尺寸检查沿用产品库的 `ls -l` 与 `awk`，适用于设备提供的精简命令环境。
mini_kfc 输出命令及返回状态，设备命令的标准输出需从设备日志查看。
上述步骤验证迁移和复制；kernel 的实际加载、启动及数据搬运通过 08 示例测试。
示例由产品库按本次运行的独立名称自动部署，手动复制文件供部署链排查使用。

### 清理手动测试文件

完成检查后，仅删除本次手动复制的目标文件，再结束迁移进程：

```bash
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" "rm -f '$DEVICE_SO'"
kill "$DEPLOY_PID"
wait "$DEPLOY_PID"
```

若 deployer 已到保活时间并退出，可直接回收后台进程状态。CUST 源文件由 CANN 管理。

## 回退与审计

回退代码提交并重新构建、部署主库与 AICPU 三份产物，可恢复原有部署流程。
修复仅删除本次生成并完成归属标记的设备文件。排查时保存部署日志及目标文件路径；
检查首次自动部署、后续重新部署、部署失败返回和释放后的文件状态。

CUST 迁移源通过当前 die、当前进程的专属链接读取，迁移前保留 CANN 已建立引用的文件。
memfabric 创建的 CP1 目标文件在失败或正常释放时清理；CANN 文件的回收时机需按运行环境确认。
