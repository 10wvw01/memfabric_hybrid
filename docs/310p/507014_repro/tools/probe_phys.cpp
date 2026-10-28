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
#include <cstdlib>
#include <cstdint>
#include "acl/acl.h"
#include "acl/acl_rt.h"
int main(int argc, char **argv)
{
    const int dev = (argc > 1) ? atoi(argv[1]) : 0;
    const int phys = (argc > 2) ? atoi(argv[2]) : dev;
    const uint64_t addr = 0x17ffbe000000ULL;
    const size_t size = 0x2000000;
    printf("probe dev(logical)=%d setAccess_phys=%d\n", dev, phys);
    printf("aclInit=%d\n", aclInit(nullptr));
    printf("setDevice=%d\n", aclrtSetDevice(dev));
    void *base = nullptr;
    printf("reserve=%d\n", aclrtReserveMemAddress(&base, size, 0, reinterpret_cast<void *>(addr), 0));
    aclrtPhysicalMemProp prop{};
    prop.handleType = aclrtMemHandleType::ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = aclrtMemAllocationType::ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = aclrtMemAttr::ACL_MEM_P2P_HUGE;
    prop.location.id = static_cast<uint32_t>(dev);
    prop.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    aclrtDrvMemHandle h = nullptr;
    printf("mallocPhysical=%d\n", aclrtMallocPhysical(&h, size, &prop, 0));
    printf("mapMem=%d\n", aclrtMapMem(reinterpret_cast<void *>(addr), size, 0, h, 0));
    aclrtMemAccessDesc acc{};
    acc.flags = aclrtMemAccessFlags::ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
    acc.location.id = static_cast<uint32_t>(phys);
    acc.location.type = aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE;
    printf("setAccess(phys=%d)=%d\n", phys, aclrtMemSetAccess(reinterpret_cast<void *>(addr), size, &acc, 1));
    return 0;
}
