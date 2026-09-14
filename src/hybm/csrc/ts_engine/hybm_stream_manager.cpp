/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/

#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include "dl_acl_api.h"
#include "hybm_logger.h"
#include "hybm_stream_manager.h"

namespace ock {
namespace mf {
static std::shared_mutex g_allThreadStreamMutex;
static std::unordered_map<int64_t, HybmStreamPtr> g_allThreadStreams;
HybmStreamPtr HybmStreamManager::GetThreadHybmStream(uint32_t devId)
{
    const auto thisId = Func::GetCurTid();
    {
        std::shared_lock lock(g_allThreadStreamMutex);
        auto it = g_allThreadStreams.find(thisId);
        if (it != g_allThreadStreams.end()) {
            return it->second;
        }
    }
    HybmStreamPtr hybmStream_ = nullptr;
    {
        std::unique_lock lock(g_allThreadStreamMutex);
        auto it = g_allThreadStreams.find(thisId);
        if (it != g_allThreadStreams.end()) {
            return it->second;
        }
        hybmStream_ = std::make_shared<HybmStream>(devId, 0, 0);
        auto ret = hybmStream_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("HybmStream init failed: " << ret);
            hybmStream_->Destroy();
            return nullptr;
        }
        g_allThreadStreams[thisId] = hybmStream_;
    }
    return hybmStream_;
}

void HybmStreamManager::DestroyAllThreadHybmStream()
{
    std::unique_lock<std::shared_mutex> lock_guard(g_allThreadStreamMutex);
    g_allThreadStreams.clear();
}

class AclrtStream {
public:
    explicit AclrtStream() : stream_(nullptr) {}
    ~AclrtStream()
    {
        if (stream_ != nullptr) {
            DlAclApi::AclrtDestroyStream(stream_);
        }
    }
    void *Get() const
    {
        return stream_;
    }
    void Reset(void *s)
    {
        stream_ = s;
    }

private:
    void *stream_;
};

int HybmStreamManager::SetDevice(uint32_t devId)
{
    auto ret = DlAclApi::AclrtSetDevice(devId);
    if (ret != 0) {
        BM_LOG_ERROR("aclrtSetDevice failed: " << ret);
        return BM_ERROR;
    }
    return BM_OK;
}

static thread_local AclrtStream g_threadAclStream;

void *HybmStreamManager::GetThreadAclStream()
{
    if (g_threadAclStream.Get() != nullptr) {
        return g_threadAclStream.Get();
    }
#if defined(ASCEND_NPU)
    void *stream = nullptr;
    static uint32_t ACL_STREAM_FAST_LAUNCH = 1U;
    static uint32_t ACL_STREAM_FAST_SYNC = 2U;
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&stream, 0, ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC);
    if (ret != 0) {
        BM_LOG_ERROR("create stream failed: " << ret);
        return nullptr;
    }
    g_threadAclStream.Reset(stream);
#endif
    return g_threadAclStream.Get();
}

void HybmStreamManager::ResetThreadAclStream()
{
    if (g_threadAclStream.Get() == nullptr) {
        return;
    }
    DlAclApi::AclrtDestroyStream(g_threadAclStream.Get());
    g_threadAclStream.Reset(nullptr);
}
} // namespace mf
} // namespace ock
