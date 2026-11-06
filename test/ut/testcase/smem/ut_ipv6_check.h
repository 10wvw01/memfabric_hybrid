/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED. INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef UT_IPV6_CHECK_H
#define UT_IPV6_CHECK_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// 探测当前环境 IPv6 loopback 是否可用：socket 创建失败（内核无 IPv6 支持）或
// bind ::1 失败（容器 disable_ipv6=1 时返回 EADDRNOTAVAIL）均视为不可用。
// IPv6 相关用例在不可用环境中应自防御 skip，而非报 FAILED。
inline bool IsIpv6LoopbackAvailable()
{
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    // 端口取 0 由内核分配，避免探测行为干扰其他用例的固定端口
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    ::close(fd);
    return true;
}

#endif // UT_IPV6_CHECK_H
