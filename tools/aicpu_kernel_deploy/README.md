# 310P AICPU 部署诊断工具

SHMEM 初始化和独立诊断使用同一套 AICPU 产物：`libmf_sdma_orch.so`、
`libmf_sdma_orch.json`、`libmf_sdma_orch_launch.json`。三份文件位于同一目录。
`MF_SDMA_ORCH_DIR` 可指定自动部署时的产物目录或其上级目录。

## SHMEM 自动部署

`smem_shm_init` 为本次初始化生成独立的设备 SO 文件名，经 CUST 通道迁移文件后，
执行一次 `cat 源文件 > 目标文件 && chmod 644 目标文件`，随后加载本次独立名称的 SO。
复制命令成功提交后，若同步返回 507018，则加载校验入口，并逐字节比较 Host 原始 SO
与设备 SO，校验通过后继续初始化。复制、加载或校验失败直接返回错误。

每次程序运行都重新生成文件名、复制并加载 SO。正常复制成功后直接加载；
自动部署不包含重试、固定等待、owner 标记、已有文件判断或禁止覆盖分支。
设备调度进程忽略 SIGCHLD 时，system 可能因等待子进程返回 ECHILD 而由 CANN 报告 507018。
正式代码保留进程原有的 SIGCHLD 设置。

`smem_shm_create` 绑定内存池并启动搬运 kernel。最后一个部署引用释放时，先停止 kernel，
再执行校验入口以保留已加载代码，删除本次独立名称对应的 SO，并确认目标文件已不存在。
异常退出后的临时文件与新任务使用不同名称。

日志包含 `automatic deploy begin`、`kernel deployed to CP1 search path`、
`deployed SO byte-for-byte verified`（507018 后的校验）、`deployment files absence verified`
和 `automatic deploy file removed`，同时记录具体目标路径。

## 独立部署测试

两个工具仅用于开发诊断，保持在产品安装包外。`deployer` 触发 CUST 迁移并保活；
`mini_kfc` 执行设备 shell，可通过独立校验入口确认文件内容或删除状态。

在仓库根目录执行，先加载 CANN 环境：

```bash
make -C tools/aicpu_kernel_deploy ASCEND_HOME_PATH="$ASCEND_HOME_PATH"
TOOLS_DIR="$(pwd)/tools/aicpu_kernel_deploy"
ARTIFACT_DIR=/path/to/aicpu_kernel
DEVICE_ID=2
DIE_ID=0
"$TOOLS_DIR/deployer" "$DEVICE_ID" "$ARTIFACT_DIR/libmf_sdma_orch.json" 600 >deployer.log 2>&1 &
DEPLOY_PID=$!
cat deployer.log
```

等待日志出现 `getfunc ret=0 verification=pending` 后继续。
`deployer` 报告迁移接口阶段状态，复制和校验期间保持该进程存活。
`DIE_ID` 按目标设备映射填写，设备侧源链接包含当前 die、vf 和迁移进程编号。

```bash
SOURCE_GLOB="/home/CustAiCpuUser/cust_aicpu_${DIE_ID}_*_${DEPLOY_PID}/libmf_sdma_orch.so"
NONCE=$(date +%s%N)
DEVICE_SO="/usr/lib64/aicpu_kernels/0/aicpu_kernels_device/libmf_sdma_orch_manual_${DEPLOY_PID}_${NONCE}.so"
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" \
    "cat $SOURCE_GLOB > '$DEVICE_SO' && chmod 644 '$DEVICE_SO'" \
    --expect-so "$ARTIFACT_DIR/libmf_sdma_orch.so" "$DEVICE_SO" "$NONCE"
```

`--expect-so` 在命令返回 0 或同步返回 507018 后均比较全部文件内容。
成功输出 `verification=BYTE_FOR_BYTE_CONFIRMED`。
nonce 是本次校验请求编号，由设备原样返回供 Host 核对。
校验入口直接调用文件接口并返回结果；配套 AICPU SO 需包含 `HybmSdmaVerifyDeploy`。

测试结束后删除本次文件并确认结果：

```bash
"$TOOLS_DIR/mini_kfc" "$DEVICE_ID" "rm -f '$DEVICE_SO'" \
    --expect-absent "$DEVICE_SO" "$NONCE"
kill "$DEPLOY_PID"
wait "$DEPLOY_PID"
```

`--expect-absent` 在删除前先执行校验入口，文件删除后继续使用已加载代码确认目标不存在。
成功输出 `verification=ABSENCE_CONFIRMED`。使用独立已部署校验 SO 时，
附加 `--verifier <设备侧SO基名>`。CUST 源文件由 CANN 管理。

单独执行命令的形式为 `mini_kfc <设备> "<命令>"`。任务提交成功后，sync 为 507018 时
输出 `shell_status=UNCONFIRMED`，未指定文件校验则退出码为 2；sync 为 0 则返回 0。
文件校验通过返回 0，执行或校验失败返回 1。通道加载、创建流、提交失败，以及其他同步错误均按错误处理。

## 回退与审计

回退提交后配套重建和部署 Host 库及 AICPU 产物。日志保留本次独立目标路径、原始 shell 状态、
校验请求编号和比较长度。清理限定为当前初始化记录的目标文件，删除后独立检查文件状态。
数据搬运和 AllReduce 通过对应功能测试验证。
