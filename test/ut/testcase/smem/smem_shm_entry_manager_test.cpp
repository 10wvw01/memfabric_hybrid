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
#include "smem_shm_entry_manager.h"
#undef private

using namespace ock::smem;

class SmemShmEntryManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto &mgr = SmemShmEntryManager::Instance();
        // Reset internal state for test isolation
        mgr.entryIdMap_.clear();
        mgr.ptr2EntryMap_.clear();
        mgr.inited_ = true; // Skip real Initialize (needs store connection)
    }

    void TearDown() override
    {
        auto &mgr = SmemShmEntryManager::Instance();
        mgr.ptr2EntryMap_.clear();
        mgr.entryIdMap_.clear();
        mgr.inited_ = false;
    }
};

// ======================== CreateEntryById Tests ========================

/**
 * CreateEntryById_NewId_ReturnsOkAndValidEntry
 *  - Creating an entry with a new id succeeds and returns a valid entry.
 */
TEST_F(SmemShmEntryManagerTest, CreateEntryById_NewId_ReturnsOkAndValidEntry)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr entry;
    Result ret = mgr.CreateEntryById(1, entry); // 1
    EXPECT_EQ(ret, SM_OK);
    EXPECT_NE(entry, nullptr);
    EXPECT_EQ(entry->Id(), 1U);              // 1U
    EXPECT_EQ(mgr.entryIdMap_.size(), 1U);   // 1U
    EXPECT_EQ(mgr.ptr2EntryMap_.size(), 1U); // 1U
}

/**
 * CreateEntryById_DuplicateId_ReturnsDuplicated
 *  - Creating an entry with an already-existing id returns SM_DUPLICATED_OBJECT.
 */
TEST_F(SmemShmEntryManagerTest, CreateEntryById_DuplicateId_ReturnsDuplicated)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr entry;
    ASSERT_EQ(mgr.CreateEntryById(1, entry), SM_OK);

    SmemShmEntryPtr dupEntry;
    Result ret = mgr.CreateEntryById(1, dupEntry); // 1
    EXPECT_EQ(ret, SM_DUPLICATED_OBJECT);
    EXPECT_EQ(dupEntry, nullptr); // out param not set on failure
}

/**
 * CreateEntryById_MultipleIds_EachReturnsDistinctEntry
 *  - Multiple different ids each create distinct entries.
 */
TEST_F(SmemShmEntryManagerTest, CreateEntryById_MultipleIds_EachReturnsDistinctEntry)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr entry1;
    SmemShmEntryPtr entry2;
    SmemShmEntryPtr entry3;

    EXPECT_EQ(mgr.CreateEntryById(10, entry1), SM_OK); // 10
    EXPECT_EQ(mgr.CreateEntryById(20, entry2), SM_OK); // 20
    EXPECT_EQ(mgr.CreateEntryById(30, entry3), SM_OK); // 30

    EXPECT_NE(entry1, nullptr);
    EXPECT_NE(entry2, nullptr);
    EXPECT_NE(entry3, nullptr);
    EXPECT_NE(entry1, entry2);
    EXPECT_NE(entry1, entry3);
    EXPECT_NE(entry2, entry3);
    EXPECT_EQ(entry1->Id(), 10U);
    EXPECT_EQ(entry2->Id(), 20U);
    EXPECT_EQ(entry3->Id(), 30U);
    EXPECT_EQ(mgr.entryIdMap_.size(), 3U);
}

/**
 * CreateEntryById_NotInited_ReturnsError
 *  - When the manager is not initialized, CreateEntryById returns SM_NOT_STARTED.
 */
TEST_F(SmemShmEntryManagerTest, CreateEntryById_NotInited_ReturnsError)
{
    auto &mgr = SmemShmEntryManager::Instance();
    mgr.inited_ = false;

    SmemShmEntryPtr entry;
    Result ret = mgr.CreateEntryById(1, entry); // 1
    EXPECT_NE(ret, SM_OK);
    EXPECT_EQ(entry, nullptr);
}

// ======================== GetEntryById Tests ========================

/**
 * GetEntryById_ExistingId_ReturnsEntry
 *  - Looking up an existing id returns the correct entry.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryById_ExistingId_ReturnsEntry)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr created;
    ASSERT_EQ(mgr.CreateEntryById(42, created), SM_OK); // 42

    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryById(42, found); // 42
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(found, created);
    EXPECT_EQ(found->Id(), 42U);
}

/**
 * GetEntryById_NonExistentId_ReturnsNotFound
 *  - Looking up a non-existent id returns SM_OBJECT_NOT_EXISTS.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryById_NonExistentId_ReturnsNotFound)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryById(999, found); // 999
    EXPECT_EQ(ret, SM_OBJECT_NOT_EXISTS);
}

/**
 * GetEntryById_NotInited_ReturnsError
 *  - Calling GetEntryById when not initialized returns SM_NOT_STARTED.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryById_NotInited_ReturnsError)
{
    auto &mgr = SmemShmEntryManager::Instance();
    mgr.inited_ = false;

    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryById(1, found); // 1
    EXPECT_NE(ret, SM_OK);
}

// ======================== GetEntryByPtr Tests ========================

/**
 * GetEntryByPtr_ExistingPtr_ReturnsEntry
 *  - Looking up by an existing raw pointer returns the correct entry.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryByPtr_ExistingPtr_ReturnsEntry)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr created;
    ASSERT_EQ(mgr.CreateEntryById(7, created), SM_OK); // 7

    uintptr_t rawPtr = reinterpret_cast<uintptr_t>(created.Get());
    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryByPtr(rawPtr, found);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(found, created);
}

/**
 * GetEntryByPtr_NonExistentPtr_ReturnsNotFound
 *  - Looking up by a pointer that was never registered returns SM_OBJECT_NOT_EXISTS.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryByPtr_NonExistentPtr_ReturnsNotFound)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryByPtr(0xDEADBEEF, found);
    EXPECT_EQ(ret, SM_OBJECT_NOT_EXISTS);
}

/**
 * GetEntryByPtr_NotInited_ReturnsError
 *  - Calling GetEntryByPtr when not initialized returns SM_NOT_STARTED.
 */
TEST_F(SmemShmEntryManagerTest, GetEntryByPtr_NotInited_ReturnsError)
{
    auto &mgr = SmemShmEntryManager::Instance();
    mgr.inited_ = false;

    SmemShmEntryPtr found;
    Result ret = mgr.GetEntryByPtr(0x1234, found);
    EXPECT_NE(ret, SM_OK);
}

// ======================== RemoveEntryByPtr Tests ========================

/**
 * RemoveEntryByPtr_ExistingPtr_RemovesAndReturnsOk
 *  - Removing an existing entry succeeds and removes it from both maps.
 */
TEST_F(SmemShmEntryManagerTest, RemoveEntryByPtr_ExistingPtr_RemovesAndReturnsOk)
{
    auto &mgr = SmemShmEntryManager::Instance();
    SmemShmEntryPtr entry;
    ASSERT_EQ(mgr.CreateEntryById(5, entry), SM_OK); // 5
    ASSERT_EQ(mgr.entryIdMap_.size(), 1U);

    uintptr_t rawPtr = reinterpret_cast<uintptr_t>(entry.Get());
    Result ret = mgr.RemoveEntryByPtr(rawPtr);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_TRUE(mgr.entryIdMap_.empty());
    EXPECT_TRUE(mgr.ptr2EntryMap_.empty());
}

/**
 * RemoveEntryByPtr_NonExistentPtr_ReturnsNotFound
 *  - Removing a non-existent pointer returns SM_OBJECT_NOT_EXISTS.
 */
TEST_F(SmemShmEntryManagerTest, RemoveEntryByPtr_NonExistentPtr_ReturnsNotFound)
{
    auto &mgr = SmemShmEntryManager::Instance();
    Result ret = mgr.RemoveEntryByPtr(0xFFFFFFFF);
    EXPECT_EQ(ret, SM_OBJECT_NOT_EXISTS);
}

/**
 * RemoveEntryByPtr_NotInited_ReturnsError
 *  - Calling RemoveEntryByPtr when not initialized returns SM_NOT_STARTED.
 */
TEST_F(SmemShmEntryManagerTest, RemoveEntryByPtr_NotInited_ReturnsError)
{
    auto &mgr = SmemShmEntryManager::Instance();
    mgr.inited_ = false;

    Result ret = mgr.RemoveEntryByPtr(0x1234);
    EXPECT_NE(ret, SM_OK);
}

// ======================== Lifecycle Tests ========================

/**
 * CreateThenGetThenRemove_FullLifecycle_Success
 *  - Full CRUD lifecycle: create → get by id → get by ptr → remove → not found after.
 */
TEST_F(SmemShmEntryManagerTest, CreateThenGetThenRemove_FullLifecycle_Success)
{
    auto &mgr = SmemShmEntryManager::Instance();

    SmemShmEntryPtr entry;
    ASSERT_EQ(mgr.CreateEntryById(100, entry), SM_OK); // 100
    ASSERT_NE(entry, nullptr);

    // Get by ID
    SmemShmEntryPtr byId;
    EXPECT_EQ(mgr.GetEntryById(100, byId), SM_OK); // 100
    EXPECT_EQ(byId, entry);

    // Get by Ptr
    SmemShmEntryPtr byPtr;
    uintptr_t rawPtr = reinterpret_cast<uintptr_t>(entry.Get());
    EXPECT_EQ(mgr.GetEntryByPtr(rawPtr, byPtr), SM_OK);
    EXPECT_EQ(byPtr, entry);

    // Remove
    EXPECT_EQ(mgr.RemoveEntryByPtr(rawPtr), SM_OK);

    // Should not be found anymore
    SmemShmEntryPtr afterRemoval;
    EXPECT_NE(mgr.GetEntryById(100, afterRemoval), SM_OK); // 100
    EXPECT_NE(mgr.GetEntryByPtr(rawPtr, afterRemoval), SM_OK);
}

/**
 * Destroy_ClearsAllEntries
 *  - Destroy() clears all internal maps and resets inited_ to false.
 */
TEST_F(SmemShmEntryManagerTest, Destroy_ClearsAllEntries)
{
    auto &mgr = SmemShmEntryManager::Instance();

    SmemShmEntryPtr entry1;
    SmemShmEntryPtr entry2;
    ASSERT_EQ(mgr.CreateEntryById(1, entry1), SM_OK);
    ASSERT_EQ(mgr.CreateEntryById(2, entry2), SM_OK); // 2
    ASSERT_EQ(mgr.entryIdMap_.size(), 2U);

    mgr.Destroy();

    EXPECT_TRUE(mgr.entryIdMap_.empty());
    EXPECT_TRUE(mgr.ptr2EntryMap_.empty());
    EXPECT_FALSE(mgr.inited_);
}

/**
 * Instance_Singleton_SameInstance
 *  - Multiple calls to Instance() return the same singleton.
 */
TEST_F(SmemShmEntryManagerTest, Instance_Singleton_SameInstance)
{
    auto &mgr1 = SmemShmEntryManager::Instance();
    auto &mgr2 = SmemShmEntryManager::Instance();
    EXPECT_EQ(&mgr1, &mgr2);
}
