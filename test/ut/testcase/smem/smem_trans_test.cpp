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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include <array>
#include <thread>
#include <chrono>
#include "smem_types.h"
#include "smem_store_factory.h"
#include "smem_trans.h"
#include "dl_acl_api.h"
#include "dl_api.h"
#include "hybm_mem_segment.h"
#include "smem_trans/smem_trans_entry.h"
#include "smem_trans/smem_trans_entry_manager.h"
#include "hybm_va_manager.h"
#include "ut_ipv6_check.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::smem;
using namespace ock::mf;

const char STORE_URL[] = "tcp://127.0.0.1:5432";
const char UNIQUE_ID[] = "127.0.0.1:5321";
const char STORE_URL_IPV6[] = "tcp://[::1]:5432";
const char UNIQUE_IPV6_ID[] = "[::1]:5321";
const uint32_t TRANS_TEST_WAIT_TIME = 1; // 1s
constexpr size_t REGISTER_MEM_ELEM_COUNT = 10;
constexpr int MAX_TEST_ATTEMPTS = 10;
constexpr int STORE_CREATE_FAILED_EXIT_CODE = 10;
constexpr int STREAM_COUNT_MISMATCH_EXIT_CODE = 10;
constexpr int STORE_CREATE_RETRY_FAILED_EXIT_CODE = 11;
const smem_trans_config_t g_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
const uint8_t MASK_MOCK_VAL = 12; // BIT_LOCAL_DEVICE | BIT_GLOBAL_DEVICE

class SmemTransTest : public testing::Test {
public:
    static void SetUpTestCase();
    static void TearDownTestCase();
    void SetUp() override;
    void TearDown() override;
};

void SmemTransTest::SetUpTestCase() {}

void SmemTransTest::TearDownTestCase() {}

void SmemTransTest::SetUp() {}

void SmemTransTest::TearDown()
{
    GlobalMockObject::verify();
    GlobalMockObject::reset();
}

TEST_F(SmemTransTest, smem_trans_config_init_success)
{
    smem_trans_config_t config;
    EXPECT_EQ(smem_trans_config_init(&config), SM_OK);
}

TEST_F(SmemTransTest, smem_trans_config_init_failed_invalid_param)
{
    EXPECT_EQ(smem_trans_config_init(nullptr), SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_create_success)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        smem_tls_config tls;
        tls.tlsEnable = false;
        ock::smem::StoreFactory::SetTlsInfo(tls);

        int32_t ret = smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        if (ret != 0) {
            _exit(1u);
        }

        ret = smem_trans_init(&g_trans_options);
        if (ret != 0) {
            _exit(2u);
        }

        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &g_trans_options);
        if (handle == nullptr) {
            _exit(3u);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(0);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_create_success_ipv6)
{
    if (!IsIpv6LoopbackAvailable()) {
        GTEST_SKIP() << "IPv6 loopback is disabled in this environment";
    }
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        smem_tls_config tls;
        tls.tlsEnable = false;
        ock::smem::StoreFactory::SetTlsInfo(tls);

        int32_t ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        if (ret != 0) {
            exit(1);
        }

        ret = smem_trans_init(&g_trans_options);
        if (ret != 0) {
            exit(2);
        }

        auto handle = smem_trans_create(STORE_URL_IPV6, UNIQUE_IPV6_ID, &g_trans_options);
        if (handle == nullptr) {
            exit(3);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);

        exit(0);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_create_failed_invalid_param)
{
    // not initialized
    EXPECT_EQ(smem_trans_create(nullptr, UNIQUE_ID, &g_trans_options), nullptr);

    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, 0);

    // storeUrl == nullptr
    EXPECT_EQ(smem_trans_create(nullptr, UNIQUE_ID, &g_trans_options), nullptr);
    // uniqueId == nullptr
    EXPECT_EQ(smem_trans_create(STORE_URL, nullptr, &g_trans_options), nullptr);
    // config == nullptr
    EXPECT_EQ(smem_trans_create(STORE_URL, UNIQUE_ID, nullptr), nullptr);

    // storeUrl or uniqueId is empty
    const char STORE_URL_TEST1[] = "";
    const char UNIQUE_ID_TEST[] = "";
    EXPECT_EQ(smem_trans_create(STORE_URL_TEST1, UNIQUE_ID, &g_trans_options), nullptr);
    EXPECT_EQ(smem_trans_create(STORE_URL, UNIQUE_ID_TEST, &g_trans_options), nullptr);
    EXPECT_EQ(smem_trans_create(STORE_URL_IPV6, UNIQUE_ID_TEST, &g_trans_options), nullptr);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_invalid_param)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        EXPECT_EQ(ret, 0);
        // client connect to server when initializing
        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &g_trans_options);

        // handle = nullptr
        ret = smem_trans_register_mem(nullptr, address, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 1;
            goto cleanup;
        }
        // address = nullptr
        ret = smem_trans_register_mem(handle, nullptr, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // size = 0
        ret = smem_trans_register_mem(handle, address, 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }

    cleanup:
        delete[] address;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_invalid_param_ipv6)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        EXPECT_EQ(ret, 0);
        // client connect to server when initializing
        auto handle = smem_trans_create(STORE_URL_IPV6, UNIQUE_IPV6_ID, &g_trans_options);

        // handle = nullptr
        ret = smem_trans_register_mem(nullptr, address, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 1;
            goto cleanup;
        }
        // address = nullptr
        ret = smem_trans_register_mem(handle, nullptr, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // size = 0
        ret = smem_trans_register_mem(handle, address, 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }

    cleanup:
        delete[] address;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_not_initialized)
{
    int *address = new int[REGISTER_MEM_ELEM_COUNT];
    size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);
    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);

    EXPECT_EQ(smem_trans_register_mem(handle, address, size, 0), SM_INVALID_PARAM);

    delete[] address;
}

TEST_F(SmemTransTest, smem_trans_register_mem_invalid_handle_nonnull)
{
    smem_trans_config_t config = g_trans_options;
    ASSERT_EQ(smem_trans_init(&config), 0);

    int *address = new int[REGISTER_MEM_ELEM_COUNT];
    size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0x12345678);

    int32_t ret = smem_trans_register_mem(invalid_handle, address, size, 0);

    EXPECT_NE(ret, SM_OK);

    delete[] address;
    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_duplicate_address)
{
    pid_t pid = fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        if (ret != 0) {
            _exit(1);
        }
        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &g_trans_options);
        if (handle == nullptr) {
            _exit(2);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret != SM_OK) {
            _exit(3);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret == SM_OK) {
            _exit(4);
        }
        smem_trans_deregister_mem(handle, address);
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        delete[] address;
        _exit(0);
    }

    int status;
    ASSERT_NE(waitpid(pid, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "Child exited with error code: " << WEXITSTATUS(status);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_success)
{
    pid_t pid = fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        if (ret != SM_OK) {
            _exit(1);
        }
        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &g_trans_options);
        if (handle == nullptr) {
            _exit(2);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret != SM_OK) {
            _exit(3);
        }
        /* 反注册已注册的内存应成功 */
        ret = smem_trans_deregister_mem(handle, address);
        if (ret != SM_OK) {
            _exit(4);
        }
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        delete[] address;
        _exit(0);
    }

    int status;
    ASSERT_NE(waitpid(pid, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "Child exited with error code: " << WEXITSTATUS(status);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_not_initialized)
{
    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);
    int dummy = 42;

    EXPECT_EQ(smem_trans_deregister_mem(handle, &dummy), SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    int dummy = 42;

    EXPECT_EQ(smem_trans_deregister_mem(nullptr, &dummy), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_null_address)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);

    EXPECT_EQ(smem_trans_deregister_mem(handle, nullptr), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_not_initialized)
{
    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_EQ(smem_trans_read(reinterpret_cast<smem_trans_t>(0x1000), local_buf, REMOTE_ID, remote_buf, 1, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_read_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_EQ(smem_trans_read(nullptr, local_buf, REMOTE_ID, remote_buf, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    char local_buf[1];
    const char remote_buf[1] = {0};
    auto handle = reinterpret_cast<smem_trans_t>(0x1000);

    EXPECT_EQ(smem_trans_read(handle, local_buf, nullptr, remote_buf, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_get_entry_failed)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADBEEF);
    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_NE(smem_trans_read(invalid_handle, local_buf, REMOTE_ID, remote_buf, 1, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_not_initialized)
{
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read(reinterpret_cast<smem_trans_t>(0x2000), local_addrs, REMOTE_ID, remote_addrs,
                                    data_sizes, 1, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read(nullptr, local_addrs, REMOTE_ID, remote_addrs, data_sizes, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x2000);
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};

    EXPECT_EQ(smem_trans_batch_read(handle, local_addrs, nullptr, remote_addrs, data_sizes, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_get_entry_failed)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xBADBAD);
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_NE(smem_trans_batch_read(invalid_handle, local_addrs, REMOTE_ID, remote_addrs, data_sizes, 1, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_not_initialized)
{
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(reinterpret_cast<smem_trans_t>(0x3000), data, REMOTE_ID, remote, 4, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(nullptr, data, REMOTE_ID, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x3000);
    const char data[] = "test";
    char remote[10] = {0};
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(handle, data, nullptr, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_stream)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x3000);
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_write_submit(handle, data, REMOTE_ID, remote, 4, nullptr, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_get_entry_failed)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADCAFE);
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x5678);

    EXPECT_NE(smem_trans_write_submit(invalid_handle, data, REMOTE_ID, remote, 4, stream, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_not_initialized)
{
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    int32_t ret =
        smem_trans_write(reinterpret_cast<smem_trans_t>(0x3000), data, REMOTE_ID, remote, sizeof(data) - 1, 0);

    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_write_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    ret = smem_trans_write(nullptr, data, REMOTE_ID, remote, sizeof(data) - 1, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    auto dummy_handle = reinterpret_cast<smem_trans_t>(0x3000);

    ret = smem_trans_write(dummy_handle, data, nullptr, remote, sizeof(data) - 1, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_get_entry_failed)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADCAFE);

    ret = smem_trans_write(invalid_handle, data, REMOTE_ID, remote, sizeof(data) - 1, 0);
    EXPECT_NE(ret, SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_not_initialized)
{
    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1111);

    EXPECT_EQ(smem_trans_read_submit(reinterpret_cast<smem_trans_t>(0x4000), local, REMOTE_ID, remote, 4, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2222);

    EXPECT_EQ(smem_trans_read_submit(nullptr, local, REMOTE_ID, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x4000);
    char local[10] = {0};
    const char remote[] = "data";
    void *stream = reinterpret_cast<void *>(0x3333);

    EXPECT_EQ(smem_trans_read_submit(handle, local, nullptr, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_stream)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x4000);
    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_read_submit(handle, local, REMOTE_ID, remote, 4, nullptr, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "127.0.0.1:9999";
    void *stream = reinterpret_cast<void *>(0x5555);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADBEEF);

    int32_t result = smem_trans_read_submit(invalid_handle, local, REMOTE_ID, remote, sizeof(remote) - 1, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_not_initialized)
{
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1111);

    EXPECT_EQ(smem_trans_batch_write_submit(reinterpret_cast<smem_trans_t>(0x6000), localAddrs, REMOTE_ID, remoteAddrs,
                                            dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2222);

    EXPECT_EQ(smem_trans_batch_write_submit(nullptr, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x6000);
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    void *stream = reinterpret_cast<void *>(0x3333);

    EXPECT_EQ(smem_trans_batch_write_submit(handle, localAddrs, nullptr, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_stream)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x6000);
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_write_submit(handle, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, nullptr, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    const size_t batchSize = 2;
    int localBuf0[10] = {1, 2, 3};
    int localBuf1[10] = {4, 5, 6};
    const void *localAddrs[batchSize] = {localBuf0, localBuf1};

    int remoteBuf0[10] = {0};
    int remoteBuf1[10] = {0};
    void *remoteAddrs[batchSize] = {remoteBuf0, remoteBuf1};

    size_t dataSizes[batchSize] = {sizeof(int) * 3, sizeof(int) * 3};
    const char *remoteUniqueId = "127.0.0.1:8888";
    void *stream = reinterpret_cast<void *>(0x7777);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xBADCAFE);

    int32_t result = smem_trans_batch_write_submit(invalid_handle, localAddrs, remoteUniqueId, remoteAddrs, dataSizes,
                                                   batchSize, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid (but non-null) handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_not_initialized)
{
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2001);

    EXPECT_EQ(smem_trans_batch_read_submit(reinterpret_cast<smem_trans_t>(0xA000), localAddrs, REMOTE_ID, remoteAddrs,
                                           dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2002);

    EXPECT_EQ(smem_trans_batch_read_submit(nullptr, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0xA001);
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    void *stream = reinterpret_cast<void *>(0x2003);

    EXPECT_EQ(smem_trans_batch_read_submit(handle, localAddrs, nullptr, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_stream)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0xA002);
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read_submit(handle, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, nullptr, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    const uint32_t batchSize = 2;
    int localBuf0[10] = {0};
    int localBuf1[10] = {0};
    void *localAddrs[batchSize] = {localBuf0, localBuf1};

    const int remoteBuf0[10] = {1, 2, 3};
    const int remoteBuf1[10] = {4, 5, 6};
    const void *remoteAddrs[batchSize] = {remoteBuf0, remoteBuf1};

    size_t dataSizes[batchSize] = {sizeof(int) * 3, sizeof(int) * 3};
    const char *remoteUniqueId = "127.0.0.1:9999";
    void *stream = reinterpret_cast<void *>(0x8888);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xF00DBEEF);

    int32_t result = smem_trans_batch_read_submit(invalid_handle, localAddrs, remoteUniqueId, remoteAddrs, dataSizes,
                                                  batchSize, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid (non-null) handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_quant_write_failed_not_initialized)
{
    smem_trans_quant_copy_param_t params = {};
    params.remoteUniqueId = "remote";
    auto handle = reinterpret_cast<smem_trans_t>(0xC000);

    EXPECT_EQ(smem_trans_batch_quant_write(handle, &params), SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_quant_write_failed_null_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    smem_trans_quant_copy_param_t params = {};
    params.remoteUniqueId = "remote";

    EXPECT_EQ(smem_trans_batch_quant_write(nullptr, &params), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_quant_write_failed_null_params)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0xC001);

    EXPECT_EQ(smem_trans_batch_quant_write(handle, nullptr), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_quant_write_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(&g_trans_options);
    ASSERT_EQ(ret, SM_OK);

    smem_trans_quant_copy_param_t params = {};
    params.remoteUniqueId = nullptr;
    auto handle = reinterpret_cast<smem_trans_t>(0xC002);

    EXPECT_EQ(smem_trans_batch_quant_write(handle, &params), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_quant_write_invalid_nonnull_handle)
{
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, SM_OK);

    const uint32_t batchSize = 1;

    float local_data[16] = {1.0f, 2.0f, 3.0f, 4.0f};
    int8_t remote_data[16] = {0};

    void *local_addr_array[1] = {local_data};
    void *remote_addr_array[1] = {remote_data};

    size_t data_sizes[1] = {sizeof(local_data)};

    float scale_val = 1.25f;
    float offset_val = 0.0f;
    float *scale_ptr_array[1] = {&scale_val};
    float *offset_ptr_array[1] = {&offset_val};

    smem_trans_quant_copy_param_t params{};
    params.remoteUniqueId = "127.0.0.1:7777";
    params.localAddrs = local_addr_array;
    params.remoteAddrs = remote_addr_array;
    params.dataSizes = data_sizes;
    params.scale = scale_ptr_array;
    params.offset = offset_ptr_array;
    params.batchSize = batchSize;
    params.unitNum = 16;
    params.stream = nullptr;
    params.inputType = 1;
    params.flags = 0;

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xC0FFEE);

    int32_t result = smem_trans_batch_quant_write(invalid_handle, &params);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid (non-null) handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_write)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    size_t capacities = 500 * sizeof(int);
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options, std::vector<int *> addrPtrs,
                   size_t capacities, const std::array<const char *, 2> unique_ids) {
        if (rank == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        trans_options.dataOpType = SMEMB_DATA_OP_SDMA;
        MOCKER_CPP(&HybmVaManager::ClassifyAddressMask, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(&trans_options);
        if (ret != 0) {
            _exit(1);
        }
        void *handle = nullptr;
        constexpr int kCreateMaxRetry = 120;
        constexpr int kRetryIntervalMs = 100;
        for (int retry = 0; retry < kCreateMaxRetry; ++retry) {
            handle = smem_trans_create(STORE_URL, unique_ids[rank], &trans_options);
            if (handle != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
        }
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_register_mem(handle, addrPtrs[rank], capacities, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        if (rank == 0) {
            // Receiver registration and session publication are asynchronous across processes.
            // Retry briefly to avoid transient "session not found" flakiness.
            constexpr int kMaxRetry = 120;
            for (int retry = 0; retry < kMaxRetry; ++retry) {
                ret = smem_trans_write(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
        }

        ret = smem_trans_deregister_mem(handle, addrPtrs[rank]);
        if (ret != SM_OK) {
            _exit(5);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"127.0.0.1:5321", "127.0.0.1:5322"}};
    std::vector<int *> addrPtrs = {sender_buffer, recv_buffer};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int kMaxAttempts = MAX_TEST_ATTEMPTS;
    for (int attempt = 0; attempt < kMaxAttempts && !testSuccess; ++attempt) {
        pid_t pids[rankSize] = {0};
        bool allExitedZero = true;
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                allExitedZero = false;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int kStoreCreateMaxRetry = 120;
                    constexpr int kRetryIntervalMs = 100;
                    for (int retry = 0; retry < kStoreCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids);
                _exit(0);
            }
        }

        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }

        for (uint32_t i = 0; i < rankSize; ++i) {
            int status = 0;
            waitpid(pids[i], &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                allExitedZero = false;
                for (uint32_t j = 0; j < rankSize; ++j) {
                    if (i != j && pids[j] > 0) {
                        kill(pids[j], SIGKILL);
                        int killedStatus = 0;
                        waitpid(pids[j], &killedStatus, 0);
                    }
                }
                break;
            }
        }
        testSuccess = allExitedZero;
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_write_ipv6)
{
    if (!IsIpv6LoopbackAvailable()) {
        GTEST_SKIP() << "IPv6 loopback is disabled in this environment";
    }
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    size_t capacities = 500 * sizeof(int);
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options, std::vector<int *> addrPtrs,
                   size_t capacities, const std::array<const char *, 2> unique_ids) {
        trans_options.dataOpType = SMEMB_DATA_OP_SDMA;
        MOCKER_CPP(&HybmVaManager::ClassifyAddressMask, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(&trans_options);
        if (ret != 0) {
            _exit(1);
        }
        constexpr int kRetryIntervalMs = 100;
        if (rank == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        void *handle = smem_trans_create(STORE_URL_IPV6, unique_ids[rank], &trans_options);
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_register_mem(handle, addrPtrs[rank], capacities, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        if (rank == 0) {
            constexpr int kWriteMaxRetry = 120;
            for (int retry = 0; retry < kWriteMaxRetry; ++retry) {
                ret = smem_trans_write(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }

            uint64_t stream_cnt = 0;
            ret = smem_trans_write_submit(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities,
                                          (void *)&stream_cnt, 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            if (stream_cnt != 1) {
                _exit(7);
            }

            ret = smem_trans_read(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
            if (ret != SM_OK) {
                _exit(8);
            }
            stream_cnt = 0;
            ret = smem_trans_read_submit(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities,
                                         (void *)&stream_cnt, 0);
            if (ret != SM_OK) {
                _exit(9);
            }
            if (stream_cnt != 1) {
                _exit(STREAM_COUNT_MISMATCH_EXIT_CODE);
            }
        }
        if (rank == 1) {
            std::this_thread::sleep_for(std::chrono::seconds(TRANS_TEST_WAIT_TIME));
        }

        ret = smem_trans_deregister_mem(handle, addrPtrs[rank]);
        if (ret != SM_OK) {
            _exit(5);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"[::]:5321", "[::]:5322"}};
    std::vector<int *> addrPtrs = {sender_buffer, recv_buffer};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int kMaxAttempts = MAX_TEST_ATTEMPTS;
    for (int attempt = 0; attempt < kMaxAttempts && !testSuccess; ++attempt) {
        pid_t pids[rankSize] = {0};
        bool allExitedZero = true;
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                allExitedZero = false;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int kStoreCreateMaxRetry = 120;
                    constexpr int kRetryIntervalMs = 100;
                    for (int retry = 0; retry < kStoreCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids);
                _exit(0);
            }
        }
        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }
        for (uint32_t i = 0; i < rankSize; ++i) {
            int status = 0;
            waitpid(pids[i], &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                allExitedZero = false;
                for (uint32_t j = 0; j < rankSize; ++j) {
                    if (i != j && pids[j] > 0) {
                        kill(pids[j], SIGKILL);
                        int killedStatus = 0;
                        waitpid(pids[j], &killedStatus, 0);
                    }
                }
                break;
            }
        }
        testSuccess = allExitedZero;
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_read_write)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    std::vector<void *> sender_addrPtrs = {sender_buffer};
    std::vector<void *> recv_addrPtrs = {recv_buffer};
    std::vector<size_t> capacities = {500 * sizeof(int)};
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options,
                   std::vector<std::vector<void *>> addrPtrs, std::vector<size_t> capacities,
                   const std::array<const char *, 2> unique_ids) {
        trans_options.dataOpType = SMEMB_DATA_OP_SDMA;
        MOCKER_CPP(&HybmVaManager::ClassifyAddressMask, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(&trans_options);
        if (ret != 0) {
            _exit(1);
        }
        auto handle = smem_trans_create(STORE_URL, unique_ids[rank], &trans_options);
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_batch_register_mem(handle, addrPtrs[rank].data(), capacities.data(), 1, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        if (rank == 0) {
            const void *srcAddr[] = {addrPtrs[0][0]};
            constexpr int kWriteMaxRetry = 120;
            constexpr int kRetryIntervalMs = 100;
            for (int retry = 0; retry < kWriteMaxRetry; ++retry) {
                ret =
                    smem_trans_batch_write(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }

            uint64_t stream_cnt = 0;
            ret = smem_trans_batch_write_submit(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(),
                                                1, (void *)&stream_cnt, 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            if (stream_cnt != 1) {
                _exit(7);
            }

            for (int retry = 0; retry < kWriteMaxRetry; ++retry) {
                ret =
                    smem_trans_batch_read(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
            stream_cnt = 0;
            ret = smem_trans_batch_read_submit(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1,
                                               (void *)&stream_cnt, 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            if (stream_cnt != 1) {
                _exit(7);
            }
        }
        if (rank == 1) {
            std::this_thread::sleep_for(std::chrono::seconds(TRANS_TEST_WAIT_TIME));
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        _exit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"127.0.0.1:5321", "127.0.0.1:5322"}};
    std::vector<std::vector<void *>> addrPtrs = {sender_addrPtrs, recv_addrPtrs};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    pid_t pids[rankSize];
    uint32_t maxProcess = rankSize;
    bool needKillOthers = false;
    for (uint32_t i = 0; i < rankSize; ++i) {
        pids[i] = fork();
        EXPECT_NE(pids[i], -1);
        if (pids[i] == -1) {
            maxProcess = i;
            needKillOthers = true;
            break;
        }
        if (pids[i] == 0) {
            smem_set_conf_store_tls(false, nullptr, 0);
            if (i == 0) {
                int ret = -1;
                constexpr int kStoreCreateMaxRetry = 120;
                constexpr int kRetryIntervalMs = 100;
                for (int retry = 0; retry < kStoreCreateMaxRetry; ++retry) {
                    ret = smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
                    if (ret == 0) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
                }
                if (ret != 0) {
                    _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
                }
            }
            func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids);
            _exit(0);
        }
    }

    if (needKillOthers) {
        for (uint32_t i = 0; i < maxProcess; ++i) {
            int status = 0;
            kill(pids[i], SIGKILL);
            waitpid(pids[i], &status, 0);
        }
        ASSERT_NE(needKillOthers, true);
    }

    for (uint32_t i = 0; i < rankSize; ++i) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        EXPECT_EQ(WIFEXITED(status), true);
        if (WIFEXITED(status)) {
            EXPECT_EQ(WEXITSTATUS(status), 0);
            if (WEXITSTATUS(status) != 0 && !needKillOthers) {
                needKillOthers = true;
                for (uint32_t j = 0; j < rankSize; ++j) {
                    if (i != j && pids[j] > 0) {
                        kill(pids[j], SIGKILL);
                        int killedStatus = 0;
                        waitpid(pids[j], &killedStatus, 0);
                    }
                }
            }
        } else {
            needKillOthers = true;
            for (uint32_t j = 0; j < rankSize; ++j) {
                if (i != j && pids[j] > 0) {
                    kill(pids[j], SIGKILL);
                    int killedStatus = 0;
                    waitpid(pids[j], &killedStatus, 0);
                }
            }
        }
    }
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_write_ipv6)
{
    if (!IsIpv6LoopbackAvailable()) {
        GTEST_SKIP() << "IPv6 loopback is disabled in this environment";
    }
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    std::vector<void *> sender_addrPtrs = {sender_buffer};
    std::vector<void *> recv_addrPtrs = {recv_buffer};
    std::vector<size_t> capacities = {500 * sizeof(int)};
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options,
                   std::vector<std::vector<void *>> addrPtrs, std::vector<size_t> capacities,
                   const std::array<const char *, 2> unique_ids) {
        trans_options.dataOpType = SMEMB_DATA_OP_SDMA;
        MOCKER_CPP(&HybmVaManager::ClassifyAddressMask, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(&trans_options);
        if (ret != 0) {
            _exit(1);
        }
        constexpr int kRetryIntervalMs = 100;
        if (rank == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        void *handle = smem_trans_create(STORE_URL_IPV6, unique_ids[rank], &trans_options);
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_batch_register_mem(handle, addrPtrs[rank].data(), capacities.data(), 1, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        if (rank == 0) {
            const void *srcAddr[] = {addrPtrs[0][0]};
            constexpr int kWriteMaxRetry = 120;
            for (int retry = 0; retry < kWriteMaxRetry; ++retry) {
                ret =
                    smem_trans_batch_write(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
        }
        if (rank == 1) {
            std::this_thread::sleep_for(std::chrono::seconds(TRANS_TEST_WAIT_TIME));
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"[::]:5321", "[::]:5322"}};
    std::vector<std::vector<void *>> addrPtrs = {sender_addrPtrs, recv_addrPtrs};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int kMaxAttempts = 5;
    for (int attempt = 0; attempt < kMaxAttempts && !testSuccess; ++attempt) {
        pid_t pids[rankSize] = {0};
        bool allExitedZero = true;
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                allExitedZero = false;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int kStoreCreateMaxRetry = 120;
                    constexpr int kRetryIntervalMs = 100;
                    for (int retry = 0; retry < kStoreCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids);
                _exit(0);
            }
        }
        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }
        for (uint32_t i = 0; i < rankSize; ++i) {
            int status = 0;
            waitpid(pids[i], &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                allExitedZero = false;
                for (uint32_t j = 0; j < rankSize; ++j) {
                    if (i != j && pids[j] > 0) {
                        kill(pids[j], SIGKILL);
                    }
                }
                break;
            }
        }
        testSuccess = allExitedZero;
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_write_failed_invalid_param)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *srcPtr1 = new int[1000];
        int *srcPtr2 = new int[2000];
        std::vector<const void *> srcAddrPtrs = {srcPtr1, srcPtr2};
        int *destPtr1 = new int[5000];
        int *destPtr2 = new int[6000];
        std::vector<void *> destAddrPtrs = {destPtr1, destPtr2};
        std::vector<size_t> dataSizes = {128U, 128U};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &g_trans_options);

        // handle = nullptr
        ret = smem_trans_batch_write(nullptr, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // srcAddresses = nullptr
        ret = smem_trans_batch_write(handle, nullptr, UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }
        // destUniqueId = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), nullptr, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 4;
            goto cleanup;
        }
        // destAddresses = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, nullptr, dataSizes.data(), dataSizes.size(),
                                     0);
        if (ret != SM_INVALID_PARAM) {
            flag = 5;
            goto cleanup;
        }
        // dataSizes = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), nullptr,
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 6;
            goto cleanup;
        }
        // batchSize = 0
        ret =
            smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(), 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 7;
            goto cleanup;
        }

    cleanup:
        delete[] srcPtr1;
        delete[] srcPtr2;
        delete[] destPtr1;
        delete[] destPtr2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        smem_destroy_config_store(STORE_URL);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_failed_invalid_param_ipv6)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *srcPtr1 = new int[1000];
        int *srcPtr2 = new int[2000];
        std::vector<const void *> srcAddrPtrs = {srcPtr1, srcPtr2};
        int *destPtr1 = new int[5000];
        int *destPtr2 = new int[6000];
        std::vector<void *> destAddrPtrs = {destPtr1, destPtr2};
        std::vector<size_t> dataSizes = {128U, 128U};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(STORE_URL_IPV6, UNIQUE_IPV6_ID, &g_trans_options);

        // handle = nullptr
        ret = smem_trans_batch_write(nullptr, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // srcAddresses = nullptr
        ret = smem_trans_batch_write(handle, nullptr, UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }
        // destUniqueId = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), nullptr, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 4;
            goto cleanup;
        }
        // destAddresses = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, nullptr, dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 5;
            goto cleanup;
        }
        // dataSizes = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), nullptr,
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 6;
            goto cleanup;
        }
        // batchSize = 0
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 7;
            goto cleanup;
        }

    cleanup:
        delete[] srcPtr1;
        delete[] srcPtr2;
        delete[] destPtr1;
        delete[] destPtr2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        smem_destroy_config_store(STORE_URL);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mems_success_receiver)
{
    smem_trans_config_t trans_options = g_trans_options;
    trans_options.role = SMEM_TRANS_RECEIVER;
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address1 = new int[1000];
        int *address2 = new int[2000];
        std::vector<void *> addrPtrs = {address1, address2};
        std::vector<size_t> capacities = {1000 * sizeof(int), 2000 * sizeof(int)};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(&g_trans_options);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(STORE_URL, UNIQUE_ID, &trans_options);

        ret = smem_trans_batch_register_mem(handle, addrPtrs.data(), capacities.data(), capacities.size(), 0);
        if (ret != SM_OK) {
            flag = 2;
            goto cleanup;
        }

    cleanup:
        delete[] address1;
        delete[] address2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mems_success_receiver_ipv6)
{
    if (!IsIpv6LoopbackAvailable()) {
        GTEST_SKIP() << "IPv6 loopback is disabled in this environment";
    }
    smem_trans_config_t trans_options = g_trans_options;
    trans_options.role = SMEM_TRANS_RECEIVER;

    int *address1 = new int[1000];
    int *address2 = new int[2000];
    std::vector<void *> addrPtrs = {address1, address2};
    std::vector<size_t> capacities = {1000 * sizeof(int), 2000 * sizeof(int)};

    // first create server
    smem_set_conf_store_tls(false, nullptr, 0);
    smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
    int ret = smem_trans_init(&g_trans_options);
    EXPECT_EQ(ret, 0);

    // client connect to server when initializing
    auto handle = smem_trans_create(STORE_URL_IPV6, UNIQUE_IPV6_ID, &trans_options);

    ret = smem_trans_batch_register_mem(handle, addrPtrs.data(), capacities.data(), capacities.size(), 0);
    EXPECT_EQ(ret, SM_OK);

    delete[] address1;
    delete[] address2;
    smem_trans_destroy(handle, 0);
    smem_trans_uninit(0);
}

// ===== CombineMemories / AlignMemory unit tests =====

namespace {
constexpr uint64_t NPU_PAGE_SIZE_UT = 2ULL * 1024ULL * 1024ULL; // 2MB, matches AlignMemory
using MemPair = std::pair<const void *, size_t>;

// Verify merge result: count and each (start, size) pair
void ExpectMerged(const std::vector<MemPair> &got, const std::vector<std::pair<uint64_t, uint64_t>> &expect)
{
    ASSERT_EQ(got.size(), expect.size());
    for (size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ(reinterpret_cast<uint64_t>(got[i].first), expect[i].first) << "idx " << i;
        EXPECT_EQ(got[i].second, expect[i].second) << "idx " << i;
    }
}
} // namespace

// Already 2MB-aligned address/size stays unchanged
TEST_F(SmemTransTest, AlignMemory_aligned_unchanged)
{
    const void *addr = reinterpret_cast<const void *>(0x100000000ULL);
    uint64_t size = NPU_PAGE_SIZE_UT * 10;
    SmemTransEntry::AlignMemory(addr, size);
    EXPECT_EQ(reinterpret_cast<uint64_t>(addr), 0x100000000ULL);
    EXPECT_EQ(size, NPU_PAGE_SIZE_UT * 10);
}

// Unaligned address: start rounds down, size rounds up and covers the prefix
TEST_F(SmemTransTest, AlignMemory_unaligned_rounded)
{
    const void *addr = reinterpret_cast<const void *>(0x100001000ULL); // page offset 0x1000
    uint64_t size = NPU_PAGE_SIZE_UT;                                  // spans two 2MB pages
    SmemTransEntry::AlignMemory(addr, size);
    EXPECT_EQ(reinterpret_cast<uint64_t>(addr), 0x100000000ULL);
    // diff(0x1000) + size(0x200000) = 0x201000, rounded up to 2MB = 0x400000
    EXPECT_EQ(size, NPU_PAGE_SIZE_UT * 2);
}

// Exact adjacency (end == next.start): NOT merged -- core fix of the issue
TEST_F(SmemTransTest, CombineMemories_adjacent_not_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 100},
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 100), NPU_PAGE_SIZE_UT * 50},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ExpectMerged(got, {{0x100000000ULL, NPU_PAGE_SIZE_UT * 100},
                       {0x100000000ULL + NPU_PAGE_SIZE_UT * 100, NPU_PAGE_SIZE_UT * 50}});
}

// Real overlap (end > next.start): merged into one segment
TEST_F(SmemTransTest, CombineMemories_overlap_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 4},                        // [0, 4MB)
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 2), NPU_PAGE_SIZE_UT * 4}, // [2MB, 6MB)
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 6); // [0, 6MB)
}

// With a gap: NOT merged
TEST_F(SmemTransTest, CombineMemories_disjoint_not_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 2},
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 10), NPU_PAGE_SIZE_UT * 2},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    EXPECT_EQ(got.size(), 2U);
}

// Unsorted input: sorted internally then overlapping segments merged
TEST_F(SmemTransTest, CombineMemories_unsorted_overlap_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 2), NPU_PAGE_SIZE_UT * 4},
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 4},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 6);
}

// Single input: returned as-is
TEST_F(SmemTransTest, CombineMemories_single_input)
{
    std::vector<MemPair> input = {{reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 3}};
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 3);
}

// Regression: simulate SGLang PD K/V exactly adjacent on a 2MB boundary.
// After alignment K_end == V_start; CombineMemories must yield two independent registrations
// instead of merging them into one segment crossing the heap boundary.
TEST_F(SmemTransTest, CombineMemories_pd_kv_boundary_regression)
{
    // K half: already 2MB-aligned
    const uint64_t kPtr = 0x12c9c0000000ULL;
    uint64_t kSize = 18069061632ULL; // K half size from the issue (unaligned)
    const void *kAddr = reinterpret_cast<const void *>(kPtr);
    SmemTransEntry::AlignMemory(kAddr, kSize);

    // V half: starts right after K's aligned end
    const uint64_t vPtr = kPtr + kSize;
    uint64_t vSize = 2258632704ULL; // V half size from the issue
    const void *vAddr = reinterpret_cast<const void *>(vPtr);
    SmemTransEntry::AlignMemory(vAddr, vSize);

    // Exactly adjacent after alignment
    EXPECT_EQ(reinterpret_cast<uint64_t>(kAddr) + kSize, reinterpret_cast<uint64_t>(vAddr));

    std::vector<MemPair> input = {{kAddr, kSize}, {vAddr, vSize}};
    auto got = SmemTransEntry::CombineMemories(input);

    // Expect: two independent segments; K must not cover V (merging would make size ~= 19.13GiB,
    // exceeding K's heap ~= 17GiB)
    ASSERT_EQ(got.size(), 2U) << "adjacent K/V must not merge, otherwise cross-heap registration fails";
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), reinterpret_cast<uint64_t>(kAddr));
    EXPECT_EQ(got[0].second, kSize);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[1].first), reinterpret_cast<uint64_t>(vAddr));
    EXPECT_EQ(got[1].second, vSize);
    // The merged K segment must not cover V's start
    EXPECT_LT(reinterpret_cast<uint64_t>(got[0].first) + got[0].second, reinterpret_cast<uint64_t>(got[1].first) + 1);
}
