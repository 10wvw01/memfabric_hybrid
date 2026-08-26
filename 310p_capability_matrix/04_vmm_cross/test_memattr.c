/*
 * test_memattr.c — 临时测试用例:验证 310P3 支持的所有 memAttr 类型
 *
 * 目的:逐一测试 aclrtPhysicalMemProp.memAttr 的所有枚举值,
 *       记录 MallocPhysical 和 GetAllocationGranularity 的返回值,
 *       确认 310P3 实际支持哪些内存属性类型。
 *
 * 用法:./test_memattr [dev_id]  (默认 dev 0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "acl/acl.h"

#define TEST_SIZE (4u * 1024u * 1024u)  /* 4MB 测试块 */

typedef struct {
    aclrtMemAttr attr;
    const char *name;
    const char *category;
    const char *expected;
} MemAttrInfo;

static const MemAttrInfo g_attrs[] = {
    /* 无介质前缀 - 通用类型 */
    { ACL_MEM_NORMAL,        "ACL_MEM_NORMAL",        "通用",     "应支持" },
    { ACL_MEM_HUGE,          "ACL_MEM_HUGE",          "通用",     "应支持" },
    { ACL_MEM_HUGE1G,        "ACL_MEM_HUGE1G",        "通用",     "仅 A3/A2" },
    /* 无介质前缀 - P2P 类型 */
    { ACL_MEM_P2P_NORMAL,    "ACL_MEM_P2P_NORMAL",    "P2P",      "应支持" },
    { ACL_MEM_P2P_HUGE,      "ACL_MEM_P2P_HUGE",      "P2P",      "应支持" },
    { ACL_MEM_P2P_HUGE1G,    "ACL_MEM_P2P_HUGE1G",    "P2P",      "仅 A3" },
    /* HBM 前缀 */
    { ACL_HBM_MEM_HUGE,      "ACL_HBM_MEM_HUGE",      "HBM",      "仅 350/A3/A2" },
    { ACL_HBM_MEM_NORMAL,    "ACL_HBM_MEM_NORMAL",    "HBM",      "仅 350/A3/A2" },
    { ACL_HBM_MEM_P2P_HUGE,  "ACL_HBM_MEM_P2P_HUGE",  "HBM-P2P",  "仅 A3/A2" },
    { ACL_HBM_MEM_P2P_NORMAL,"ACL_HBM_MEM_P2P_NORMAL","HBM-P2P",  "仅 A3/A2" },
    /* DDR 前缀 - Host 内存 */
    { ACL_DDR_MEM_HUGE,      "ACL_DDR_MEM_HUGE",      "DDR",      "仅 Host" },
    { ACL_DDR_MEM_NORMAL,    "ACL_DDR_MEM_NORMAL",    "DDR",      "仅 Host" },
    { ACL_DDR_MEM_P2P_HUGE,  "ACL_DDR_MEM_P2P_HUGE",  "DDR-P2P",  "仅 Host" },
    { ACL_DDR_MEM_P2P_NORMAL,"ACL_DDR_MEM_P2P_NORMAL","DDR-P2P",  "仅 Host" },
};

#define NUM_ATTRS (sizeof(g_attrs) / sizeof(g_attrs[0]))

int main(int argc, char *argv[])
{
    int32_t dev = 0;
    if (argc > 1)
        dev = atoi(argv[1]);

    printf("=== test_memattr: 验证 310P3 支持的 memAttr 类型 ===\n");
    printf("设备: dev%d, 测试大小: %uMB\n\n", dev, TEST_SIZE >> 20);

    aclError ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("aclInit FAIL ret=%d\n", ret);
        return 1;
    }

    ret = aclrtSetDevice(dev);
    if (ret != ACL_ERROR_NONE) {
        printf("aclrtSetDevice(%d) FAIL ret=%d\n", dev, ret);
        aclFinalize();
        return 1;
    }

    printf("%-24s %-10s %-14s %-10s %-10s %-12s %s\n",
           "memAttr", "类别", "文档预期", "gran_ret", "gran(B)", "malloc_ret", "结论");
    printf("------------------------------------------------------------------------------------------\n");

    int supported_count = 0;

    for (size_t i = 0; i < NUM_ATTRS; i++) {
        const MemAttrInfo *info = &g_attrs[i];

        aclrtPhysicalMemProp prop;
        memset(&prop, 0, sizeof(prop));
        prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
        prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
        prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        prop.location.id = (uint32_t)dev;
        prop.memAttr = info->attr;

        size_t gran = 0;
        aclError gr = aclrtMemGetAllocationGranularity(
            &prop, ACL_RT_MEM_ALLOC_GRANULARITY_RECOMMENDED, &gran);

        aclrtDrvMemHandle handle = NULL;
        aclError mr = aclrtMallocPhysical(&handle, TEST_SIZE, &prop, 0);

        const char *conclusion;
        if (mr == ACL_ERROR_NONE) {
            conclusion = "✓ 支持";
            supported_count++;
            aclrtFreePhysical(handle);
        } else if (mr == 100000) {
            conclusion = "✗ 不支持(100000)";
        } else {
            conclusion = "✗ 其他错误";
        }

        printf("%-24s %-10s %-14s %-10d %-10zu %-12d %s\n",
               info->name, info->category, info->expected,
               gr, gran, mr, conclusion);

        fflush(stdout);
    }

    printf("------------------------------------------------------------------------------------------\n");
    printf("总计: %zu 种类型, %d 种支持\n\n", NUM_ATTRS, supported_count);

    printf("=== 差异对比 ===\n");
    printf("1. 无介质前缀 (ACL_MEM_*): 通用类型,310P3 应支持\n");
    printf("2. HBM 前缀 (ACL_HBM_MEM_*): 仅 Atlas 350/A3/A2,310P3 不支持\n");
    printf("3. DDR 前缀 (ACL_DDR_MEM_*): 仅 Host 内存,310P3 是 Device 不适用\n");
    printf("4. P2P 类型: 用于跨设备数据复制,310P3 仅 ACL_MEM_P2P_HUGE 可用\n");
    printf("5. 1G 大页 (HUGE1G): 仅 A3 系列,310P3 不支持\n");

    aclrtResetDevice(dev);
    aclFinalize();

    return 0;
}
