# 310P AICPU 部署诊断工具

310P SHMEM 初始化时由产品库自动部署 AICPU kernel。构建产物使用固定名称：
`libmf_sdma_orch.so`、`libmf_sdma_orch.json`、`libmf_sdma_orch_launch.json`。
三份文件需要位于同一目录，可通过 `MF_SDMA_ORCH_DIR` 指定产物目录或其上级目录进行递归查找。

`smem_shm_init` 初始化配置存储和设备，并持有部署资源引用。
初始化首次 Acquire 生成独立的临时加载名称，通过 CUST 迁移 SO，以禁止覆盖的方式复制到
CP1 搜索目录，再加载校验入口，核对本次归属标记及 Host 与设备 SO 的每个字节。
`smem_shm_create` 选择 SDMA 后绑定内存池并启动搬运 kernel。
临时加载名称由运行时生成，使用者无需维护版本号。已有部署文件不会被当作本次
部署成功的依据；CP1 加载缓存按本次独立名称区分。

最终 `hybm_uninit` 释放初始化持有的引用。最后一个引用释放时，库先停止 kernel，再删除本次创建的设备文件。
其他进程及历史部署文件保持原样。异常退出可能留下临时设备文件，后续进程使用
新的名称完成部署。设备通道失败或部署失败会返回错误。

部署日志包含 `automatic deploy begin`、`deployed SO byte-for-byte verified`、
`deployment files absence verified`、`automatic deploy file removed` 和具体设备路径，可用于核对复制与清理结果。

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

通道检查成功时输出 `launch=0 sync=0`。`sync=507018` 时结果待确认，文件操作需继续检查实际状态。
其他错误先排查设备状态、CANN 环境和 KFC 通道，再继续迁移测试。

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
DEVICE_SO="/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_manual_${DEPLOY_PID}.so"
NONCE=$(date +%s%N)
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" \
    "set -- $SOURCE_GLOB; test \$# -eq 1 && test -f \"\$1\" && (set -C; { printf %s '$NONCE' > '$DEVICE_SO.owner' && cat \"\$1\"; } > '$DEVICE_SO') && chmod 644 '$DEVICE_SO'" \
    --expect-so "$ARTIFACT_DIR/libmf_sdma_orch.so" "$DEVICE_SO" "$NONCE"
```

复制模式核对归属标记及全部文件内容，成功时输出 `verification=BYTE_FOR_BYTE_CONFIRMED`。
命令返回 507018 时仍继续校验实际结果；文件或标记不匹配时返回失败。
kernel 的启动及数据搬运通过 08 示例测试。示例由产品库按本次独立名称自动部署。

### 清理手动测试文件

完成检查后，按本次归属标记删除目标文件，并确认 SO 与标记均已不存在：

```bash
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" \
    "test \"\$(cat '$DEVICE_SO.owner')\" = '$NONCE' && rm -f '$DEVICE_SO' '$DEVICE_SO.owner'" \
    --expect-absent "$DEVICE_SO" "$NONCE"
kill "$DEPLOY_PID"
wait "$DEPLOY_PID"
```

成功时输出 `verification=ABSENCE_CONFIRMED`，随后结束迁移进程。

若 deployer 已到保活时间并退出，可直接回收后台进程状态。CUST 源文件由 CANN 管理。

## 回退与审计

回退代码提交并重新构建、部署主库与 AICPU 三份产物，可恢复原有部署流程。
修复仅删除本次生成并完成归属标记的设备文件。排查时保存部署日志及目标文件路径；
检查首次自动部署、后续重新部署、部署失败返回和释放后的文件状态。

CUST 迁移源通过当前 die、当前进程的专属链接读取，迁移前保留 CANN 已建立引用的文件。
memfabric 创建的 CP1 目标文件在失败或正常释放时清理；CANN 文件的回收时机需按运行环境确认。

## shell返回码与部署确认

设备调度进程忽略SIGCHLD时，shell命令可能已经执行，但system等待子进程时返回ECHILD，
随后由CANN报告507018。库把这类返回统一记录为结果未确认，并保留原始返回码。
任务提交失败、超时和通道断开仍按错误处理。

每次部署使用独立文件名和nonce标记。首次复制采用noclobber，已有目标只在标记匹配时进入校验，
不会原地改写可能已加载的SO。加载后调用专用AICPU校验入口，核对标记，并与Host上传的原文件
逐字节比较。只有返回本次nonce、成功状态和完整比较长度后，才启动SHMEM编排器。

清理前先执行校验入口，保证校验代码已加载，再删除带本次nonce标记的文件，
随后通过同一AICPU结果通道确认SO及标记均已不存在。
校验入口直接使用文件接口并写回结果，不依赖shell的退出码。正式部署代码保持原有信号设置。

回退此改动需配套回退并重建Host库与AICPU产物。诊断时保留原始shell返回码、目标路径、nonce、
比较字节数和清理确认记录；校验失败时拒绝启动编排器。

## 手动shell命令的结果确认

`mini_kfc <dev> "<cmd>"` 保留原始launch/sync返回码。任务提交成功后，sync为507018时输出
`shell_status=UNCONFIRMED`，退出码为2；其他执行错误返回1。sync为0时返回0。

文件操作可指定实际结果检查，所用AICPU校验SO需来自本次配套构建并包含HybmSdmaVerifyDeploy入口：

```bash
mini_kfc <dev> "<复制并写入owner标记的命令>" --expect-so <Host原始SO> <设备文件名或路径> <nonce>
mini_kfc <dev> "<按nonce匹配后删除SO及owner标记的命令>" --expect-absent <设备文件名或路径> <nonce>
```

这些模式在shell返回0或507018后均检查实际状态，只有内容、标记或删除状态确认成功才返回0。
校验失败返回1。默认从目标SO中加载校验入口；清理模式在删除前先加载，使文件删除后仍可使用入口。
使用独立的已部署校验SO时，附加 `--verifier <设备侧SO基名>`。

复制命令应使用独立目标名和noclobber，并把nonce写入目标的 `.owner` 文件；校验模式逐字节比较
Host原始SO和设备文件。`--expect-absent`确认目标与 `.owner` 文件均不存在。
nonce需为本次操作生成的非零整数。`deployer`仅报告迁移接口阶段结果，实际文件验证状态输出为pending。
