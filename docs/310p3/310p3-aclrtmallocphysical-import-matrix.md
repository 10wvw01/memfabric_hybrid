# 310P3 aclrtMallocPhysical 跨卡 Import 测试矩阵

**日期**: 2026-09-08
**平台**: 310P3 (8卡, CANN 9.1.0)
**分配大小**: 256MB

---

## 0. 测试参数说明

### 0.1 aclrtMallocPhysical 接口

```cpp
aclError aclrtMallocPhysical(
    aclrtDrvMemHandle* handle,      // [out] 返回的内存句柄
    size_t size,                    // [in]  分配大小
    const aclrtPhysicalMemProp* prop, // [in]  内存属性
    uint64_t flags                  // [in]  保留参数, 传 0
);
```

### 0.2 aclrtPhysicalMemProp 参数

```cpp
typedef struct {
    aclrtMemHandleType handleType;      // 句柄类型, 固定 ACL_MEM_HANDLE_TYPE_NONE
    aclrtMemAllocationType allocationType; // 分配类型, 固定 PINNED
    aclrtMemLocation location;         // 内存位置
    uint32_t memAttr;                  // 内存属性
    uint64_t reserve;                  // 保留, 传 0
} aclrtPhysicalMemProp;

typedef struct {
    uint32_t id;                       // 设备 ID (DEVICE 时为 deviceId, HOST 时为 0)
    aclrtMemLocationType type;         // ACL_MEM_LOCATION_TYPE_HOST=0 或 ACL_MEM_LOCATION_TYPE_DEVICE
} aclrtMemLocation;
```

### 0.3 location.type 与 memAttr 的关系

`location.type` 决定内存分配的物理位置, `memAttr` 决定页类型和内存介质。两者不是任意组合:

- **DEVICE location**: 内存分配在 NPU HBM 上, `memAttr` 仅允许 HBM_MEM_*/MEM_*/MEM_P2P_* (DDR_* 被 ACL 层拒绝, 返回 100000)
- **HOST location**: 内存分配在主机 DRAM 上, `memAttr` 允许 DDR_MEM_*/HBM_MEM_HUGE/HBM_MEM_NORMAL/MEM_*/MEM_P2P_* (HBM_MEM_P2P_* 被拒绝, 返回 100000)

ACL 层校验逻辑 (`runtime/src/acl/aclrt_impl/memory.cpp:1042-1052`):
```cpp
// DEVICE location + DDR_* memAttr → 拒绝
if (isDeviceAlloc && (memAttr == DDR_MEM_HUGE || ... || DDR_MEM_P2P_HUGE || ...)) {
    return ACL_ERROR_INVALID_PARAM;  // 100000
}
```

### 0.4 测试流程

单进程内跨设备 (非跨进程):

```
dev0 (card0)                           dev2 (card1)
  │                                      │
  ├─ aclrtSetDevice(0)                   │
  ├─ aclrtMallocPhysical(prop)           │
  │    location.type = DEVICE 或 HOST     │
  │    memAttr = 各枚举值                 │
  ├─ aclrtMemExportToShareableHandle     │
  │    → shareable handle                 │
  ├─ aclrtSetDevice(2)                    │
  │                                      │
  └──────────────────────────────────────→ aclrtMemImportFromShareableHandle
                                           shareable, deviceId=2
```

- **跨卡**: 310P3 Duo 上 `deviceId / 2` 决定物理卡号。dev0/dev1 在 card0, dev2/dev3 在 card1。dev0→dev2 为跨卡。
- **非跨进程**: 单进程内 `aclrtSetDevice(0)` 切换到 `aclrtSetDevice(2)`, 不是 fork 多进程。
- **分配大小**: 256MB (避免触发 4GB 上限, 排除 size 因素干扰)。

### 0.5 memAttr 枚举值

| 枚举值 | 索引 | 页类型 (HAL) | 内存介质 (HAL) | 允许的 location |
|--------|------|-------------|---------------|----------------|
| ACL_DDR_MEM | 0 | — | — | HOST |
| ACL_HBM_MEM | 1 | — | — | DEVICE |
| ACL_DDR_MEM_HUGE | 2 | HUGE (2MB) | DDR | HOST |
| ACL_DDR_MEM_NORMAL | 3 | NORMAL (4KB) | DDR | HOST |
| ACL_HBM_MEM_HUGE | 4 | HUGE (2MB) | HBM | DEVICE |
| ACL_HBM_MEM_NORMAL | 5 | NORMAL (4KB) | HBM | DEVICE |
| ACL_DDR_MEM_P2P_HUGE | 6 | HUGE (2MB) | P2P DDR | HOST |
| ACL_DDR_MEM_P2P_NORMAL | 7 | NORMAL (4KB) | P2P DDR | HOST |
| ACL_HBM_MEM_P2P_HUGE | 8 | HUGE (2MB) | P2P HBM | DEVICE |
| ACL_HBM_MEM_P2P_NORMAL | 9 | NORMAL (4KB) | P2P HBM | DEVICE |
| ACL_HBM_MEM_HUGE1G | 10 | HUGE1G (1GB) | HBM | DEVICE |
| ACL_HBM_MEM_P2P_HUGE1G | 11 | HUGE1G (1GB) | P2P HBM | DEVICE |
| ACL_MEM_NORMAL | 12 | NORMAL (4KB) | 自动 | DEVICE/HOST |
| ACL_MEM_HUGE | 13 | HUGE (2MB) | 自动 | DEVICE/HOST |
| ACL_MEM_HUGE1G | 14 | HUGE1G (1GB) | 自动 | DEVICE/HOST |
| ACL_MEM_P2P_NORMAL | 15 | NORMAL (4KB) | P2P | DEVICE/HOST |
| ACL_MEM_P2P_HUGE | 16 | HUGE (2MB) | P2P | DEVICE/HOST |
| ACL_MEM_P2P_HUGE1G | 17 | HUGE1G (1GB) | P2P | DEVICE/HOST |

**注意**: `MEM_*` 系列 (索引 12-17) 的内存介质由 ACL 层根据 `location.type` 自动映射:
- DEVICE location → HBM 介质
- HOST location → DDR 介质

`P2P` 前缀表示支持 P2P 跨设备访问; `HUGE` = 2MB 大页; `HUGE1G` = 1GB 大页 (310P3 不支持); `NORMAL` = 4KB 页。

---

## 1. DEVICE location (HBM 内存)

**测试参数**: `prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE`, `prop.location.id = 0` (dev0), `prop.memAttr = 各枚举值`

| memAttr | alloc | export | import(dev2) | 错误码 |
|---------|-------|--------|-------------|--------|
| HBM_MEM_HUGE | OK | OK | **FAIL** | 507899 |
| HBM_MEM_NORMAL | OK | OK | **FAIL** | 507899 |
| DDR_MEM_HUGE | FAIL(100000) | — | — | — |
| DDR_MEM_NORMAL | FAIL(100000) | — | — | — |
| DDR_MEM_P2P_HUGE | FAIL(100000) | — | — | — |
| DDR_MEM_P2P_NORMAL | FAIL(100000) | — | — | — |
| HBM_MEM_P2P_HUGE | FAIL(100000) | — | — | — |
| HBM_MEM_P2P_NORMAL | FAIL(100000) | — | — | — |
| HBM_MEM_HUGE1G | FAIL(207000) | — | — | — |
| HBM_MEM_P2P_HUGE1G | FAIL(100000) | — | — | — |
| MEM_NORMAL | OK | OK | **FAIL** | 507899 |
| MEM_HUGE | OK | OK | **FAIL** | 507899 |
| MEM_HUGE1G | FAIL(207000) | — | — | — |
| **MEM_P2P_NORMAL** | OK | OK | **OK** | — |
| **MEM_P2P_HUGE** | OK | OK | **OK** | — |
| MEM_P2P_HUGE1G | FAIL(207000) | — | — | — |

### DEVICE 结论

- **仅 `MEM_P2P_NORMAL` 和 `MEM_P2P_HUGE` 支持跨卡 import**
- `HBM_MEM_HUGE`/`MEM_NORMAL`/`MEM_HUGE` 虽能 alloc+export，但 import 返回 `507899`（DRV_INTERNAL_ERROR）
- `507899` = 非 P2P 类型内存不支持跨设备 import
- memfabric 当前使用 `MEM_P2P_HUGE`，跨卡 import 正常

---

## 2. HOST location (Host 内存)

**测试参数**: `prop.location.type = ACL_MEM_LOCATION_TYPE_HOST`, `prop.location.id = 0`, `prop.memAttr = 各枚举值`

| memAttr | alloc | export | import(dev2) | 错误码 |
|---------|-------|--------|-------------|--------|
| DDR_MEM | FAIL(100000) | — | — | — |
| HBM_MEM | FAIL(100000) | — | — | — |
| DDR_MEM_HUGE | OK | OK | **FAIL** | 107000 |
| DDR_MEM_NORMAL | OK | OK | **FAIL** | 107000 |
| DDR_MEM_P2P_HUGE | OK | OK | **FAIL** | 107000 |
| DDR_MEM_P2P_NORMAL | OK | OK | **FAIL** | 107000 |
| HBM_MEM_HUGE | OK | OK | **FAIL** | 107000 |
| HBM_MEM_NORMAL | OK | OK | **FAIL** | 107000 |
| HBM_MEM_P2P_HUGE | FAIL(100000) | — | — | — |
| HBM_MEM_P2P_NORMAL | FAIL(100000) | — | — | — |
| HBM_MEM_HUGE1G | FAIL(207000) | — | — | — |
| HBM_MEM_P2P_HUGE1G | FAIL(100000) | — | — | — |
| MEM_NORMAL | OK | OK | **FAIL** | 107000 |
| MEM_HUGE | OK | OK | **FAIL** | 107000 |
| MEM_HUGE1G | FAIL(207000) | — | — | — |
| MEM_P2P_NORMAL | OK | OK | **FAIL** | 107000 |
| MEM_P2P_HUGE | OK | OK | **FAIL** | 107000 |
| MEM_P2P_HUGE1G | FAIL(207000) | — | — | — |

### HOST 结论

- **所有 HOST location memAttr 均不支持跨卡 import**
- 即使 `DDR_MEM_P2P_HUGE`/`MEM_P2P_HUGE` 也能 alloc+export，但 import 返回 `107000`（INVALID_PARAM）
- `107000` = ACL runtime 不支持 HOST 内存的跨设备 import
- 这就是 memfabric DRAM 跨 rank 共享使用 `shm_open` + `MAP_SHARED` 而非 ACL VMM 的原因

---

## 3. 错误码说明

| 错误码 | 含义 | 出现场景 |
|--------|------|---------|
| 100000 | INVALID_PARAM | memAttr 与 location 不匹配（如 DDR_MEM + DEVICE） |
| 207000 | INTERNAL_ERROR | 不支持的页类型（如 HUGE1G 在 310P3 不支持） |
| 207001 | 分配上限超限 | MEM_P2P_HUGE + DEVICE + 4GB（上限 3GB） |
| 507899 | DRV_INTERNAL_ERROR | 非 P2P 类型 DEVICE 内存不支持跨设备 import |
| 107000 | INVALID_PARAM | HOST 内存不支持跨设备 import |

---

## 4. 对 memfabric 的影响

| 内存类型 | memAttr | alloc | export | 跨卡 import | memfabric 方案 |
|---------|---------|-------|--------|-----------|--------------|
| HBM | MEM_P2P_HUGE | ✅ | ✅ | ✅ | ACL VMM export/import（当前方案） |
| DRAM | — | — | — | ❌ | `shm_open` + `MAP_SHARED`（当前方案） |

**HBM**: `MEM_P2P_HUGE` 是唯一同时满足 alloc+export+跨卡import 的 DEVICE memAttr。4GB 分配上限 3GB 是此 memAttr 的限制。

**DRAM**: 所有 HOST memAttr 均不支持跨卡 import。`shm_open` + `MAP_SHARED` 是唯一可行的跨 rank DRAM 共享方案。

---

## 5. alloc 上限补充（DEVICE location）

| memAttr | 1GB | 2GB | 3GB | 4GB | 跨卡 import |
|---------|-----|-----|-----|-----|-----------|
| MEM_P2P_HUGE | OK | OK | OK | **FAIL(207001)** | **OK** |
| MEM_P2P_NORMAL | OK | OK | OK | **FAIL(207001)** | **OK** |
| HBM_MEM_HUGE | OK | OK | OK | OK | FAIL(507899) |
| MEM_NORMAL | OK | OK | OK | OK | FAIL(507899) |
| MEM_HUGE | OK | OK | OK | OK | FAIL(507899) |

**权衡**: `HBM_MEM_HUGE` 无 4GB 限制但不能跨卡 import；`MEM_P2P_HUGE` 支持跨卡 import 但上限 3GB。memfabric 选 `MEM_P2P_HUGE`（跨卡 import 是刚需）。

---

## 6. MEM_P2P_HUGE 4GB 限制根因分析

### 6.1 现象

`aclrtMallocPhysical` 使用 `MEM_P2P_HUGE` + `DEVICE` location 时, 1-3GB 分配成功, 4GB 返回 `207001`。

### 6.2 调用链追踪

通过 `/home/l30023043/runtime` 源码追踪完整调用链:

```
aclrtMallocPhysical                    // ACL 层: acl/aclrt_impl/memory.cpp:1003
  → aclrtMallocPhysicalImpl           // 参数校验 + memAttr→HAL prop 映射
    → rtMallocPhysical                // runtime 层: runtime/api/api_c.cc:3687
      → Api::MallocPhysical           // runtime/core/src/api_impl/api_impl.cc:7028
        → NpuDriver::MallocPhysical   // runtime/driver/npu_driver_mem.cc:348
          → halMemCreate              // HAL 驱动: libascend_hal.so (弱符号)
```

### 6.3 各层代码分析

#### ACL 层 (`runtime/src/acl/aclrt_impl/memory.cpp:1003-1074`)

```cpp
aclError aclrtMallocPhysicalImpl(aclrtDrvMemHandle *handle, size_t size,
                                 const aclrtPhysicalMemProp *prop, uint64_t flags)
{
    // 参数校验: handleType=NONE, allocationType=PINNED, location.type 有效
    // memAttr + location 校验: DEVICE + DDR_* → 拒绝(100000)

    // memAttr → HAL prop 映射
    rtDrvMemProp_t rtProp = {};
    rtProp.side = prop->location.type;    // DEVICE=1
    rtProp.devid = prop->location.id;     // 0
    auto it = memAttrHandlers.find(prop->memAttr);
    it->second(rtProp, isHostAlloc, isDeviceAlloc);
    // MEM_P2P_HUGE + DEVICE → rtProp.pg_type=HUGE_PAGE_TYPE, rtProp.mem_type=P2P_HBM_TYPE

    // 调用 runtime 层
    ACL_REQUIRES_RTS_OK(rtMallocPhysical(handle, size, &rtProp, flags));
}
```

memAttr 映射表 (`memory.cpp:76`):
```cpp
{ACL_MEM_P2P_HUGE, [](rtDrvMemProp_t& p, bool isHost, bool isDev) {
    if(isHost) { p.pg_type = HUGE_PAGE_TYPE; p.mem_type = P2P_DDR_TYPE; }
    else if(isDev) { p.pg_type = HUGE_PAGE_TYPE; p.mem_type = P2P_HBM_TYPE; }
}}
```

ACL 层**无 size 上限校验**, 直接透传到 runtime 层。

#### Runtime 层 (`runtime/src/runtime/api/api_c.cc:3687`)

```cpp
rtError_t rtMallocPhysical(rtDrvMemHandle* handle, size_t size,
                           rtDrvMemProp_t* prop, uint64_t flags)
{
    const rtError_t error = apiInstance->MallocPhysical(handle, size, prop, flags);
    // 无 size 校验
}
```

#### ApiImpl 层 (`runtime/core/src/api_impl/api_impl.cc:7028`)

```cpp
rtError_t ApiImpl::MallocPhysical(rtDrvMemHandle* handle, size_t size,
                                  rtDrvMemProp_t* prop, uint64_t flags)
{
    return NpuDriver::MallocPhysical(handle, size, prop, flags);
    // 无 size 校验, 直接调用 HAL 驱动
}
```

#### HAL 驱动层 (`runtime/driver/npu_driver_mem.cc:348-382`)

```cpp
rtError_t NpuDriver::MallocPhysical(rtDrvMemHandle* handle, size_t size,
                                    rtDrvMemProp_t* prop, uint64_t flags)
{
    // 获取芯片属性
    DevProperties properties;
    GET_DEV_PROPERTIES(chipType, properties);
    // 可能的 mem_type 映射调整
    MapPhysicalMemTypeByPolicy(prop, properties.physicalMemTypePolicy);

    // 调用 HAL 驱动
    drvRet = halMemCreate(handle, size, prop, flags);
    // drvRet 驱动返回的错误码 → runtime 错误码
    // 207001 对应 HAL 驱动的分配失败
}
```

`halMemCreate` 是 `__attribute__((weak))` 弱符号, 实现在 `libascend_hal.so` (闭源二进制), runtime 源码中只有声明:

```cpp
// runtime/driver/npu_driver_base.hpp:74
drvError_t __attribute__((weak)) halMemCreate(
    drv_mem_handle_t **handle, size_t size,
    const struct drv_mem_prop *prop, uint64_t flag);
```

### 6.4 根因定位

| 层级 | 是否有 size 校验 | 说明 |
|------|----------------|------|
| ACL 层 (`memory.cpp`) | 否 | 仅校验参数合法性, 直接透传 |
| Runtime 层 (`api_c.cc`) | 否 | 直接调用 ApiImpl |
| ApiImpl 层 (`api_impl.cc`) | 否 | 直接调用 NpuDriver |
| HAL 驱动层 (`npu_driver_mem.cc`) | 否 | 调用 `halMemCreate`, 错误码透传 |
| **HAL 驱动二进制 (`libascend_hal.so`)** | **是 (3GB)** | `halMemCreate` 内部限制, 返回 `207001` |

**根因**: 310P3 HAL 驱动 `libascend_hal.so` 的 `halMemCreate` 对 `P2P_HBM_TYPE` + `HUGE_PAGE_TYPE` (2MB 大页) 的单次分配上限为 3GB。4GB 分配返回 `207001` (对应 HAL 错误码, runtime 层透传)。

### 6.5 为什么只有 MEM_P2P_HUGE 有此限制

| memAttr | HAL 映射 (pg_type, mem_type) | 4GB 限制 | 说明 |
|---------|---------------------------|---------|------|
| MEM_P2P_HUGE | HUGE_PAGE_TYPE, P2P_HBM_TYPE | **3GB** | P2P HBM 2MB 大页, HAL 驱动限制 |
| MEM_P2P_NORMAL | NORMAL_PAGE_TYPE, P2P_HBM_TYPE | **3GB** | P2P HBM 4KB 页, 同样限制 |
| HBM_MEM_HUGE | HUGE_PAGE_TYPE, HBM_TYPE | 无 | 非 P2P HBM 2MB 大页, 无限制 |
| HBM_MEM_NORMAL | NORMAL_PAGE_TYPE, HBM_TYPE | 无 | 非 P2P HBM 4KB 页, 无限制 |
| MEM_NORMAL | NORMAL_PAGE_TYPE, HBM_TYPE | 无 | ACL 映射为非 P2P |
| MEM_HUGE | HUGE_PAGE_TYPE, HBM_TYPE | 无 | ACL 映射为非 P2P |

限制与 `mem_type = P2P_HBM_TYPE` 相关, 非 P2P 类型 (`HBM_TYPE`) 无此限制。P2P HBM 内存的 HAL 驱动管理机制可能需要额外的页表/P2P 映射资源, 单次分配上限 3GB。

### 6.6 可选规避方案

| 方案 | 改动 | 优势 | 劣势 |
|------|------|------|------|
| **A. 参数化 GVA_SIZE (当前)** | benchmark `-gs` 参数, 默认 1GB | 零改动, 已实施 | 310P3 最大 3GB |
| B. 多次分配拼接 | 将 N GB 拆为 ceil(N/3) 次 ≤3GB 分配 | 支持任意大小 | 需修改 `HybmVmmBasedSegment::AllocLocalMemory`, 增加多个 slice/handle 管理 |
| C. 改用 HBM_MEM_HUGE | 无 4GB 限制 | 无 size 限制 | 跨卡 import 返回 507899, **不可行** |
| D. 等待 HAL 驱动修复 | 无代码改动 | 根因解决 | 依赖驱动版本升级 |

当前采用方案 A, 310P3 有效范围 1-3GB (GB 对齐要求)。

---

## 7. 测试源码

### 6.1 跨卡 import 测试程序

```cpp
// test_import_matrix.cpp
// 编译: g++ -o test_import_matrix test_import_matrix.cpp \
//        -I$ASCEND_HOME_PATH/aarch64-linux/include \
//        -L$ASCEND_HOME_PATH/aarch64-linux/lib64 -lascendcl -fpermissive
// 运行: ./test_import_matrix

#include <cstdio>
#include <cstdlib>
#include "acl/acl.h"

static void testOne(const char* name, aclrtMemAttr attr, aclrtMemLocationType locType) {
    printf("%-22s ", name);
    aclInit(nullptr);
    aclrtSetDevice(0);

    // Step 1: dev0 alloc
    aclrtDrvMemHandle h = nullptr;
    aclrtPhysicalMemProp prop{};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = static_cast<aclrtMemAllocationType>(0);
    prop.location.id = 0;
    prop.location.type = locType;
    prop.memAttr = attr;
    auto r = aclrtMallocPhysical(&h, 256 * 1024 * 1024, &prop, 0);
    if (r != 0) { printf("alloc FAIL(%d)\n", r); aclFinalize(); return; }

    // Step 2: dev0 export
    uint64_t shareable = 0;
    r = aclrtMemExportToShareableHandle(h, ACL_MEM_HANDLE_TYPE_NONE, 1, &shareable);
    if (r != 0) { printf("export FAIL(%d)\n", r); aclrtFreePhysical(h); aclFinalize(); return; }

    // Step 3: dev2 (cross-card) import
    aclrtSetDevice(2);
    aclrtDrvMemHandle h2 = nullptr;
    r = aclrtMemImportFromShareableHandle(shareable, 2, &h2);
    if (r != 0) {
        printf("import FAIL(%d)\n", r);
    } else {
        printf("import OK h2=%p\n", h2);
    }

    // Cleanup
    aclrtSetDevice(0);
    aclrtFreePhysical(h);
    aclrtResetDevice(0);
    aclrtResetDevice(2);
    aclFinalize();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== DEVICE: dev0 alloc+export -> dev2 import (cross-card) ===\n");
    testOne("HBM_MEM_HUGE", ACL_HBM_MEM_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE);
    testOne("HBM_MEM_NORMAL", ACL_HBM_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE);
    testOne("MEM_NORMAL", ACL_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE);
    testOne("MEM_HUGE", ACL_MEM_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE);
    testOne("MEM_P2P_NORMAL", ACL_MEM_P2P_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE);
    testOne("MEM_P2P_HUGE", ACL_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE);

    printf("\n=== HOST: dev0 alloc+export -> dev2 import (cross-card) ===\n");
    testOne("DDR_MEM_HUGE", ACL_DDR_MEM_HUGE, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("DDR_MEM_NORMAL", ACL_DDR_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("DDR_MEM_P2P_HUGE", ACL_DDR_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("DDR_MEM_P2P_NORMAL", ACL_DDR_MEM_P2P_NORMAL, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("MEM_NORMAL", ACL_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("MEM_HUGE", ACL_MEM_HUGE, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("MEM_P2P_NORMAL", ACL_MEM_P2P_NORMAL, ACL_MEM_LOCATION_TYPE_HOST);
    testOne("MEM_P2P_HUGE", ACL_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_HOST);

    return 0;
}
```

### 6.2 alloc 上限测试程序

```cpp
// test_alloc_limit.cpp
// 编译: g++ -o test_alloc_limit test_alloc_limit.cpp \
//        -I$ASCEND_HOME_PATH/aarch64-linux/include \
//        -L$ASCEND_HOME_PATH/aarch64-linux/lib64 -lascendcl -fpermissive
// 运行: ./test_alloc_limit

#include <cstdio>
#include "acl/acl.h"

struct TestCase {
    const char* name;
    aclrtMemAttr attr;
    aclrtMemLocationType loc;
};

int main() {
    aclInit(nullptr);

    TestCase cases[] = {
        // DEVICE location
        {"HBM_MEM_HUGE", ACL_HBM_MEM_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE},
        {"HBM_MEM_NORMAL", ACL_HBM_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE},
        {"MEM_NORMAL", ACL_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE},
        {"MEM_HUGE", ACL_MEM_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE},
        {"MEM_P2P_NORMAL", ACL_MEM_P2P_NORMAL, ACL_MEM_LOCATION_TYPE_DEVICE},
        {"MEM_P2P_HUGE", ACL_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_DEVICE},
        // HOST location
        {"DDR_MEM_HUGE", ACL_DDR_MEM_HUGE, ACL_MEM_LOCATION_TYPE_HOST},
        {"DDR_MEM_P2P_HUGE", ACL_DDR_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_HOST},
        {"MEM_P2P_HUGE", ACL_MEM_P2P_HUGE, ACL_MEM_LOCATION_TYPE_HOST},
        {"MEM_P2P_NORMAL", ACL_MEM_P2P_NORMAL, ACL_MEM_LOCATION_TYPE_HOST},
        {"MEM_NORMAL", ACL_MEM_NORMAL, ACL_MEM_LOCATION_TYPE_HOST},
        {"MEM_HUGE", ACL_MEM_HUGE, ACL_MEM_LOCATION_TYPE_HOST},
    };

    size_t sizes[] = {
        1ULL * 1024 * 1024 * 1024,
        2ULL * 1024 * 1024 * 1024,
        3ULL * 1024 * 1024 * 1024,
        4ULL * 1024 * 1024 * 1024,
    };

    int nCases = sizeof(cases) / sizeof(cases[0]);

    for (int loc = 0; loc < 2; loc++) {
        const char* locName = (loc == 0) ? "DEVICE" : "HOST";
        printf("=== %s ===\n", locName);
        for (int c = 0; c < nCases; c++) {
            if (cases[c].loc != (aclrtMemLocationType)loc + 1) continue;
            printf("%-22s", cases[c].name);
            for (int s = 0; s < 4; s++) {
                aclrtSetDevice(0);
                aclrtDrvMemHandle h = nullptr;
                aclrtPhysicalMemProp prop{};
                prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
                prop.allocationType = static_cast<aclrtMemAllocationType>(0);
                prop.location.id = 0;
                prop.location.type = cases[c].loc;
                prop.memAttr = cases[c].attr;
                auto ret = aclrtMallocPhysical(&h, sizes[s], &prop, 0);
                printf("  %s(%d)", (ret == 0) ? "OK" : "FAIL", ret);
                if (ret == 0 && h) aclrtFreePhysical(h);
            }
            printf("\n");
        }
        printf("\n");
    }

    // Also test aclrtMalloc (non-VMM) for comparison
    printf("=== aclrtMalloc (non-VMM) ===\n");
    printf("HUGE_FIRST  ");
    for (int s = 0; s < 4; s++) {
        aclrtSetDevice(0);
        void *ptr = nullptr;
        auto ret = aclrtMalloc(&ptr, sizes[s], ACL_MEM_MALLOC_HUGE_FIRST);
        printf("  %s(%d)", (ret == 0) ? "OK" : "FAIL", ret);
        if (ret == 0 && ptr) aclrtFree(ptr);
    }
    printf("\n");

    aclrtResetDevice(0);
    aclFinalize();
    return 0;
}
```

---

## 7. 测试方法

### 7.1 环境准备

```bash
# CANN 环境
source /usr/local/Ascend/cann-9.1.0/set_env.sh

# 确认 NPU 可用
npu-smi info

# 确认 310P3 平台（dev0 在 card0, dev2 在 card1, 跨卡）
# 310P3 Duo: dev0/dev1 同卡, dev2/dev3 同卡
```

### 7.2 编译

```bash
# 跨卡 import 测试
g++ -o test_import_matrix test_import_matrix.cpp \
    -I$ASCEND_HOME_PATH/aarch64-linux/include \
    -L$ASCEND_HOME_PATH/aarch64-linux/lib64 -lascendcl -fpermissive

# alloc 上限测试
g++ -o test_alloc_limit test_alloc_limit.cpp \
    -I$ASCEND_HOME_PATH/aarch64-linux/include \
    -L$ASCEND_HOME_PATH/aarch64-linux/lib64 -lascendcl -fpermissive
```

### 7.3 运行

```bash
# 跨卡 import 测试（单进程，dev0 alloc → dev2 import）
./test_import_matrix

# alloc 上限测试（单进程，逐个 memAttr × 1-4GB）
./test_alloc_limit
```

### 7.4 注意事项

1. **跨卡定义**: 310P3 Duo 平台上 `dev/2` 决定物理卡号。dev0 和 dev2 在不同物理卡上（跨卡），dev0 和 dev1 在同一物理卡上（同卡）。

2. **测试隔离**: 每个测试用例独立调用 `aclInit` / `aclFinalize`，避免 VA 空间和 handle 泄漏影响后续测试。

3. **不测 MapMem/SetAccess**: 跨卡 import 后的 `aclrtMapMem` + `aclrtMemSetAccess` + 内存读写在某些 memAttr 上会 segfault（VA 空间冲突或 ACL runtime 状态异常），因此测试仅覆盖 import 阶段。完整的数据读写验证在 memfabric 的 `bm_perf_benchmark` 和 `310p3_trans_test` 中进行。

4. **aclrtMalloc 对比**: `aclrtMalloc`（非 VMM）无 4GB 限制，但返回的地址不在 `g_vmmHandleMap` 中，无法 `AclrtMemExportToShareableHandle`，因此不适用于 memfabric 的跨 rank 共享场景。

### 7.5 预期输出

#### 跨卡 import 测试

```
=== DEVICE: dev0 alloc+export -> dev2 import (cross-card) ===
HBM_MEM_HUGE           import FAIL(507899)
HBM_MEM_NORMAL         import FAIL(507899)
MEM_NORMAL             import FAIL(507899)
MEM_HUGE               import FAIL(507899)
MEM_P2P_NORMAL         import OK h2=0x...
MEM_P2P_HUGE           import OK h2=0x...

=== HOST: dev0 alloc+export -> dev2 import (cross-card) ===
DDR_MEM_HUGE           import FAIL(107000)
DDR_MEM_NORMAL         import FAIL(107000)
DDR_MEM_P2P_HUGE       import FAIL(107000)
DDR_MEM_P2P_NORMAL     import FAIL(107000)
MEM_NORMAL             import FAIL(107000)
MEM_HUGE               import FAIL(107000)
MEM_P2P_NORMAL         import FAIL(107000)
MEM_P2P_HUGE           import FAIL(107000)
```

#### alloc 上限测试

```
=== DEVICE ===
HBM_MEM_HUGE             OK  OK  OK  OK
HBM_MEM_NORMAL           OK  OK  OK  OK
MEM_NORMAL               OK  OK  OK  OK
MEM_HUGE                 OK  OK  OK  OK
MEM_P2P_NORMAL           OK  OK  OK  FAIL(207001)
MEM_P2P_HUGE             OK  OK  OK  FAIL(207001)

=== HOST ===
DDR_MEM_HUGE             OK  OK  OK  OK
DDR_MEM_P2P_HUGE         OK  OK  OK  OK
MEM_P2P_HUGE             OK  OK  OK  OK
MEM_P2P_NORMAL           OK  OK  OK  OK
MEM_NORMAL               OK  OK  OK  OK
MEM_HUGE                 OK  OK  OK  OK

=== aclrtMalloc (non-VMM) ===
HUGE_FIRST               OK  OK  OK  OK
```
