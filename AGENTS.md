## 构建

- **默认**：`bash script/build_and_pack_run.sh` → RELEASE，Python ON，XPU_TYPE=NPU。
- **Debug**：`bash script/build_and_pack_run.sh --build_mode DEBUG`
- **无加速器**：`bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_hcom_rdma OFF`
- **HCOM + RDMA**：`--build_hcom ON --build_hcom_rdma ON`
- **etcd 后端**：`--build_etcd_backend ON`
- **Bazel**：`--build_tool bazel`（CMake 为默认）。
- 优先使用 wrapper 而非 `script/build.sh`（位置参数易混淆）。build.sh 默认值：
  BUILD_OPEN_ABI=OFF（旧 C++11 ABI），BUILD_PYTHON=ON，ENABLE_PTRACER=ON，XPU_TYPE=NPU，
  HCOM/etcd 禁用，MF_BUILD_JOBS=32。

## 测试

- **全部 UT**：`bash script/run_ut.sh` — 带覆盖率的干净 ASAN 构建（lines≥70%，branches≥40%）。
- **快速（增量，无覆盖率）**：`bash script/run_ut.sh --fast`
- **单个测试**：`bash script/run_ut.sh --fast TestName`（传递 `--gtest_filter=*TestName*`）
- **首次运行前需初始化子模块**：`git submodule update --recursive --init`（获取 `test/3rdparty/googletest` 和 `test/3rdparty/mockcpp`）。
- **强制要求**：commit 或提交 PR 前，必须跑过完整 UT（`bash script/run_ut.sh`）且全部通过
  （环境限制导致的 SKIPPED 不算失败）。不允许只跑单个用例通过就直接提交；
  不得提交任何已知失败的用例。

## 预提交 / 代码风格

- **安装**：`pip install pre-commit && pre-commit install --install-hooks`
- **CI PR 检查**：`bash script/ci-pre-commit-pr.sh`
- **Python**：ruff 目标 `py310`，行长度 120。配置位于 `pre-commit/pyproject.toml`。
- **C++**：clang-format v18.1.8，基于 Google，IndentWidth=4，ColumnLimit=120，Allman 大括号。配置位于 `.clang-format`。

## PR / Issue 操作

- **优先用 GitCode CLI（`gc`）**：获取/创建/修改 PR、issue 等操作，先确认 `gc auth status` 已登录；
  已登录则用 `gc pr`、`gc issue`、`gc api` 等命令完成，不手写裸 API 调用
- **回退**：`gc` 未安装或未登录时，再考虑 SSH / HTTPS 方式（`git push` 推分支 + 网页建 PR）

## 代码规范（强制要求）

以下规范必须遵守，违反将导致代码被拒绝：

### 函数行数限制

- 新增函数尽量≤50行（非空非注释行）
- **修改现有代码时，尽量不超过50行**
- 超过50行尽量拆分为多个函数
- 拆分原则：按功能模块划分，保持逻辑清晰

### 嵌套深度限制

- 嵌套深度≤4层（if/for/while/switch等控制块）
- 超过4层必须重构：提取函数、提前return、合并条件

### 单一职责原则

- 每个函数只完成一个明确功能
- 避免"万能函数"（如同时做初始化+计算+输出）
- 函数名应清晰表达其单一职责

### 白话表述纪律

- 注释、文档、提交信息一律用白话直述功能，**禁止使用比喻性黑话**（如"看门狗"）、**禁止自造新词、禁止打比方**
- 用功能描述代替：如"周期性读取设备运行状态并打印的后台线程"，而不是"看门狗"；"状态字"，而不是"判定词"式自造词
- 已有仓库术语（邮箱、邮件、请求环、到达环、完成戳等）沿用，不新增同类造词

### 注释规范

- 解释"为什么"，不重复"做了什么"；代码能说清的（命名、结构、常量）不注释
- 公共接口、复杂逻辑、边界条件必须注释
- 注释随代码同步维护，过时即删；不保留注释掉的代码
- `TODO/FIXME` 写清原因或关联单号
- 只写能降低理解成本的内容；不引用 issue 单号、不保留历史取证信息

### 错误返回日志

- 新增代码中出现 `return` 错误返回路径时，必须记录 ERROR 级别日志
- 根错误发生处必须记录失败原因和关键参数，确保日志能定位问题
- 上层仅透传下层已记录过的错误时，可不强制重复记录 ERROR 日志
- 禁止只返回错误而完全没有任何一层记录根因日志
- 选择关键参数时必须贴合本项目语义，禁止照搬本项目不存在的泛业务 ID 示例

关键参数优先级：

| 优先级 | 参数类型 | MemFabric 示例 |
|--------|---------|----------------|
| 高 | 标识符 | `rankId`、`localRank`、`remoteRank`、`deviceId`、`pid`、`threadId`、`channel`、`handle` |
| 高 | 资源名/资源句柄 | `shmName`、`shmKey`、`storeKey`、`bufferId`、`memHandle`、队列名、文件名 |
| 高 | 通信/任务定位信息 | `traceId`、`requestId`、`jobId`、`batchTag`、`opId`（仅在代码中真实存在时） |
| 中 | 路径/端点 | 文件路径、`ASCEND_MF_STORE_URL`、etcd endpoint、TCP/HCOM/RDMA 地址和端口 |
| 中 | 配置项 | 超时时间、重试次数、阈值、`worldSize`、`rankSize`、`opType`、`xpu_type`、HCOM/RDMA 开关 |
| 中 | 外部调用目标 | ACL/URMA/HCOM/etcd 接口名、下游服务名、RPC 方法名、`smem_bm_*` / `hybm_*` 调用点 |
| 中 | 状态码/错误码 | `ret`、`errno`、`StoreErrorCode`、`BM_*` 错误码、ACL/HCOM/URMA 错误码、退出码 |
| 低 | 内部中间值 | `idx`、`offset`、`batchSize`、`len`/`length`、`srcVA`/`dstVA`（仅在有助于定位时） |

### 行宽限制

- 行宽≤120字符
- 超长需合理换行，在运算符后截断
- 换行后后续行与运算符对齐

## 运行时 / 配置

- **配置存储 URL**（`ASCEND_MF_STORE_URL`）必填：`tcp://...`、`etcd://...` 或 `reg://...`。
- **NPU**：`ASCEND_HOME_PATH` 必须指向 CANN/Ascend 安装目录。
- **GPU**：`CUDA_HOME` 必须指向 CUDA 安装目录。
- **Python 日志级别**：`ASCEND_MF_LOG_LEVEL`（0-4）。
- 完整环境变量列表：`doc/environment_variables.md`。

## 仓库模块边界

| 模块 | 路径 | 产物 |
|------|------|------|
| SMEM API (BM/TRANS/SHM) | `src/smem/` | `libmf_smem.so` |
| 内存管理 / 传输 | `src/hybm/` | `libmf_hybm_core.so` |
| IPC 控制通道 | `src/acc_links/` | — |
| etcd Go 后端 | `src/util/etcd_client/etcd_store_backend/` | shared lib |
| 公共 C 头文件 (SMEM) | `src/smem/include/host/` | — |
| 公共头文件 (hybm) | `include/hybm/` | — |
| 官方应用 | `app/zbal/`、`app/tensorRTL/` | — |
| 示例 | `examples/` | — |

## 产物 / 依赖

- **前置依赖**：CMake ≥3.12（推荐 3.20+），GCC ≥11.4，GLIBC ≥2.28，Ninja/Make 4.3+，Python ≥3.8，pybind11 ≥2.10.3。
- **可选依赖**：libibverbs-dev（HCOM RDMA）、Go（etcd 后端）、auditwheel（Python wheel）。
- **Python wheel 构建**：需设置 `MEMFABRIC_VERSION`；`XPU_TYPE` 追加 cpu/gpu 标签；`IS_MANYLINUX` 控制平台标签。
- **忽略生成产物**：`build/`、`output/`、`compile_commands.json`、`wheels/`、shared libs — 这些是构建产物，非源码。

## 许可证

Mulan PSL v2 — 参见文件头部和 LICENSE 文件。
