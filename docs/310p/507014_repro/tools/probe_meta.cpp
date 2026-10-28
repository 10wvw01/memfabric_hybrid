/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
 * 用法: ASCEND_RT_VISIBLE_DEVICES=2 ./probe_meta [devId]
 */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "acl/acl.h"
#include "acl/acl_rt.h"

int main(int argc, char **argv)
{
    const int dev = (argc > 1) ? atoi(argv[1]) : 0;
    const uint64_t addr = (argc > 4) ? strtoull(argv[4], nullptr, 0) : 0x17ffbe000000ULL;
    const void *expect = (addr == 0) ? nullptr : reinterpret_cast<void *>(addr);
    const size_t size = 0x2000000;
    printf("probe dev=%d addr=0x%llx size=0x%zx aclInit=%d\n", dev, (unsigned long long)addr, size,
           aclInit(nullptr));
    if (argc > 2) {
        printf("aclrtSetDevice=%d\n", aclrtSetDevice(dev));
    }
    if (argc > 3) {
        int32_t ena = aclrtDeviceEnablePeerAccess(atoi(argv[3]), 0);
        printf("enablePeerAccess(%d)=%d\n", atoi(argv[3]), ena);
    }
    void *base = nullptr;
    int32_t r = aclrtReserveMemAddress(&base, size, 0, const_cast<void *>(expect), 0);
    printf("reserve=%d base=%p\n", r, base);
    if (r != 0) return 1;
    aclrtPhysicalMemProp prop{};
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = aclrtMemAllocationType::ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = aclrtMemAttr::ACL_MEM_P2P_HUGE;
    prop.location.id = static_cast<uint32_t>(dev);
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    aclrtDrvMemHandle h = nullptr;
    r = aclrtMallocPhysical(&h, size, &prop, 0);
    printf("mallocPhysical=%d h=%p\n", r, h);
    if (r != 0) return 1;
    r = aclrtMapMem(base, size, 0, h, 0);
    printf("mapMem=%d\n", r);
    if (r != 0) return 1;
    aclrtMemAccessDesc acc{};
    acc.flags = aclrtMemAccessFlags::ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    acc.location.id = static_cast<uint32_t>(dev);
    acc.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    r = aclrtMemSetAccess(base, size, &acc, 1);
    printf("setAccess(dev)=%d\n", r);
    aclrtMemAccessDesc accH{};
    accH.flags = aclrtMemAccessFlags::ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    accH.location.id = 0;
    accH.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_HOST;
    aclrtMemAccessDesc both[2] = {acc, accH};
    r = aclrtMemSetAccess(base, size, both, 2);
    printf("setAccess(dev+host)=%d\n", r);
    return 0;
}
