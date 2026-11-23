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

#include <gtest/gtest.h>

#define private public
#include "smem_shm_entry.h"
#undef private

using namespace ock::smem;

class SmemShmEntryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        entry_ = std::make_shared<SmemShmEntry>(42); // 42
    }

    void TearDown() override
    {
        entry_.reset();
    }

    std::shared_ptr<SmemShmEntry> entry_;
};

TEST_F(SmemShmEntryTest, Id_ReturnsConfiguredId)
{
    EXPECT_EQ(entry_->Id(), 42U);
}

TEST_F(SmemShmEntryTest, GetGva_ReturnsNullInitially)
{
    EXPECT_EQ(entry_->GetGva(), nullptr);
}

TEST_F(SmemShmEntryTest, GetHbmMaxSize_ReturnsZeroInitially)
{
    EXPECT_EQ(entry_->GetHbmMaxSize(), 0U);
}

TEST_F(SmemShmEntryTest, GetGroup_ReturnsNullInitially)
{
    EXPECT_EQ(entry_->GetGroup(), nullptr);
}

TEST_F(SmemShmEntryTest, SetExtraContext_NotInited_ReturnsError)
{
    entry_->inited_ = false;
    entry_->entity_ = nullptr;
    auto ret = entry_->SetExtraContext(nullptr, 0);
    EXPECT_EQ(ret, SM_ERROR);
}

TEST_F(SmemShmEntryTest, SetConfig_DoesNotCrash)
{
    smem_shm_config_t config{};
    smem_shm_config_init(&config);
    config.controlOperationTimeout = 30; // 30
    EXPECT_NO_THROW(entry_->SetConfig(config));
}

TEST_F(SmemShmEntryTest, GetReachInfo_NullEntity_ReturnsNotStarted)
{
    entry_->entity_ = nullptr;
    uint32_t reachInfo = 0;
    auto ret = entry_->GetReachInfo(0, reachInfo); // 0
    EXPECT_EQ(ret, SM_NOT_STARTED);
}

TEST_F(SmemShmEntryTest, ProducerMaskDefaultsAndConfigAreRetained)
{
    EXPECT_EQ(entry_->extraConfig_.sdmaProducerMask, 0xFFU);
    smem_shm_config_t config{};
    ASSERT_EQ(smem_shm_config_init(&config), 0);
    config.sdmaProducerMask = 0x7FFF;
    entry_->SetConfig(config);
    EXPECT_EQ(entry_->extraConfig_.sdmaProducerMask, 0x7FFFU);
}
