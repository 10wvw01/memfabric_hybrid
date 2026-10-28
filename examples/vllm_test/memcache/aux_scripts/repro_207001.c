/*
 * Minimal repro for AclrtMallocPhysical ret:207001 (RT_ERROR_DRV_OUT_MEMORY)
 *
 * Reproduces the failure seen in memcache_npu_test at:
 *   hybm_vmm_based_segment.cpp:233 MallocFromDevice
 *   AclrtMallocPhysical failed, ret:207001 devId:2 size:1073741824
 *
 * This demo directly calls the ACL VMM API chain:
 *   aclInit -> aclrtSetDevice -> aclrtReserveMemAddress
 *   -> aclrtMallocPhysical(ACL_MEM_P2P_HUGE)   [expected: 207001 on dev2/dev3]
 *   -> aclrtMallocPhysical(ACL_HBM_MEM_HUGE)   [fallback, expected: success]
 *
 * Build:  gcc -o repro_207001 repro_207001.c -lascendcl
 * Usage:  ./repro_207001 [device_id]    (default: 0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <acl/acl.h>

#define ALLOC_SIZE ((size_t)(1ULL * 1024 * 1024 * 1024))
#define PASS 0
#define FAIL 1

static const char *mem_attr_name(aclrtMemAttr attr)
{
    switch (attr) {
        case ACL_MEM_P2P_HUGE:    return "ACL_MEM_P2P_HUGE";
        case ACL_HBM_MEM_HUGE:    return "ACL_HBM_MEM_HUGE";
        case ACL_HBM_MEM:         return "ACL_HBM_MEM";
        case ACL_MEM_HUGE:        return "ACL_MEM_HUGE";
        default:                  return "UNKNOWN";
    }
}

static int try_malloc_physical(uint32_t deviceId, aclrtMemAttr attr, size_t size)
{
    aclrtPhysicalMemProp prop;
    memset(&prop, 0, sizeof(prop));
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.id = deviceId;
    prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    prop.memAttr = attr;

    aclrtDrvMemHandle handle = NULL;
    aclError ret = aclrtMallocPhysical(&handle, size, &prop, 0);
    printf("  aclrtMallocPhysical(%s) size=%zu ret=%d\n", mem_attr_name(attr), size, ret);

    if (ret != ACL_SUCCESS) {
        printf("  -> FAILED (ret=%d)\n", ret);
        if (ret == 207001) {
            printf("  -> 207001 = RT_ERROR_DRV_OUT_MEMORY (P2P partition exhausted)\n");
        }
        return FAIL;
    }

    printf("  -> SUCCESS, handle=%p\n", handle);
    aclrtFreePhysical(handle);
    return PASS;
}

int main(int argc, char *argv[])
{
    uint32_t deviceId = 0;
    if (argc > 1) {
        deviceId = (uint32_t)atoi(argv[1]);
    }
    printf("=== repro_207001: deviceId=%u, size=%zu ===\n\n", deviceId, ALLOC_SIZE);

    /* Step 1: aclInit */
    aclError ret = aclInit(NULL);
    if (ret != ACL_SUCCESS && ret != 100000) {
        printf("[FAIL] aclInit ret=%d\n", ret);
        return FAIL;
    }
    printf("[OK] aclInit\n");

    /* Step 2: aclrtSetDevice */
    ret = aclrtSetDevice(deviceId);
    if (ret != ACL_SUCCESS) {
        printf("[FAIL] aclrtSetDevice(%u) ret=%d\n", deviceId, ret);
        return FAIL;
    }
    printf("[OK] aclrtSetDevice(%u)\n\n", deviceId);

    /* Step 3: aclrtReserveMemAddress (reserve VA, should always succeed) */
    void *va = NULL;
    ret = aclrtReserveMemAddress(&va, ALLOC_SIZE, 0, NULL, 0);
    if (ret != ACL_SUCCESS) {
        printf("[FAIL] aclrtReserveMemAddress ret=%d\n", ret);
        return FAIL;
    }
    printf("[OK] aclrtReserveMemAddress va=%p size=%zu\n\n", va, ALLOC_SIZE);

    /* Step 4: aclrtMallocPhysical with ACL_MEM_P2P_HUGE (the failing path) */
    printf("--- Step 4: P2P_HUGE (memcache_npu_test path) ---\n");
    int p2p_result = try_malloc_physical(deviceId, ACL_MEM_P2P_HUGE, ALLOC_SIZE);
    printf("\n");

    /* Step 5: aclrtMallocPhysical with ACL_HBM_MEM_HUGE (fallback path) */
    printf("--- Step 5: HBM_MEM_HUGE (fallback) ---\n");
    int hbm_result = try_malloc_physical(deviceId, ACL_HBM_MEM_HUGE, ALLOC_SIZE);
    printf("\n");

    /* Step 6: also test ACL_HBM_MEM (non-huge) for completeness */
    printf("--- Step 6: HBM_MEM (non-huge) ---\n");
    int hbm_plain_result = try_malloc_physical(deviceId, ACL_HBM_MEM, ALLOC_SIZE);
    printf("\n");

    /* Summary */
    printf("========== SUMMARY ==========\n");
    printf("  ACL_MEM_P2P_HUGE : %s\n", p2p_result == PASS ? "PASS" : "FAIL (207001)");
    printf("  ACL_HBM_MEM_HUGE : %s\n", hbm_result == PASS ? "PASS" : "FAIL");
    printf("  ACL_HBM_MEM      : %s\n", hbm_plain_result == PASS ? "PASS" : "FAIL");
    printf("=============================\n");

    if (p2p_result == FAIL) {
        printf("\nReproduced: ACL_MEM_P2P_HUGE returns 207001 on deviceId=%u\n", deviceId);
        printf("This matches memcache_npu_test failure at hybm_vmm_based_segment.cpp:233\n");
    }

    aclrtReleaseMemAddress(va);
    aclrtResetDevice(deviceId);
    aclFinalize();
    return p2p_result;
}
