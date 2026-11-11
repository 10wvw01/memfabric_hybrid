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
} MemAttrInfo;

static const MemAttrInfo g_attrs[] = {
    { ACL_MEM_NORMAL, "ACL_MEM_NORMAL" },
    { ACL_MEM_HUGE, "ACL_MEM_HUGE" },
    { ACL_MEM_HUGE1G, "ACL_MEM_HUGE1G" },
    { ACL_MEM_P2P_NORMAL, "ACL_MEM_P2P_NORMAL" },
    { ACL_MEM_P2P_HUGE, "ACL_MEM_P2P_HUGE" },
    { ACL_MEM_P2P_HUGE1G, "ACL_MEM_P2P_HUGE1G" },
    { ACL_HBM_MEM_HUGE, "ACL_HBM_MEM_HUGE" },
    { ACL_HBM_MEM_NORMAL, "ACL_HBM_MEM_NORMAL" },
    { ACL_HBM_MEM_P2P_HUGE, "ACL_HBM_MEM_P2P_HUGE" },
    { ACL_HBM_MEM_P2P_NORMAL, "ACL_HBM_MEM_P2P_NORMAL" },
    { ACL_DDR_MEM_HUGE, "ACL_DDR_MEM_HUGE" },
    { ACL_DDR_MEM_NORMAL, "ACL_DDR_MEM_NORMAL" },
    { ACL_DDR_MEM_P2P_HUGE, "ACL_DDR_MEM_P2P_HUGE" },
    { ACL_DDR_MEM_P2P_NORMAL, "ACL_DDR_MEM_P2P_NORMAL" },
};

static int probe_attr(const MemAttrInfo *info, int32_t dev)
{
    aclrtPhysicalMemProp prop = {0};
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
    printf("OBSERVE memAttr=%s gran_ret=%d gran=%zu malloc_ret=%d\n", info->name, gr, gran, mr);
    if (mr != ACL_ERROR_NONE)
        return 0;
    aclError ret = aclrtFreePhysical(handle);
    if (ret != ACL_ERROR_NONE)
        fprintf(stderr, "FAIL aclrtFreePhysical attr=%s device=%d ret=%d\n", info->name, dev, ret);
    return ret != ACL_ERROR_NONE;
}

int main(int argc, char *argv[])
{
    int32_t dev = argc > 1 ? atoi(argv[1]) : 0;
    aclError ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "FAIL aclInit ret=%d\n", ret);
        return 1;
    }
    ret = aclrtSetDevice(dev);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "FAIL aclrtSetDevice device=%d ret=%d\n", dev, ret);
        aclFinalize();
        return 1;
    }
    int failed = 0;
    for (size_t i = 0; i < sizeof(g_attrs) / sizeof(g_attrs[0]); ++i)
        failed |= probe_attr(&g_attrs[i], dev);
    ret = aclrtResetDevice(dev);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "FAIL aclrtResetDevice device=%d ret=%d\n", dev, ret);
        failed = 1;
    }
    ret = aclFinalize();
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "FAIL aclFinalize ret=%d\n", ret);
        failed = 1;
    }
    printf("RESULT memattr_probe %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
