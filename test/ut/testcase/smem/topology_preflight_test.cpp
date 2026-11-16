#include <gtest/gtest.h>

#include "smem_store_factory.h"
#include "topology_preflight.h"

using namespace ock::smem;

class TopologyPreflightTest : public testing::Test {
protected:
    void SetUp() override
    {
        smem_tls_config tls{};
        tls.tlsEnable = false;
        StoreFactory::SetTlsInfo(tls);
        store_ = StoreFactory::CreateStoreByUrl(url_, ConfigStoreModel::CSM_BOTH);
        ASSERT_NE(store_, nullptr);
    }

    void TearDown() override
    {
        store_ = nullptr;
        StoreFactory::DestroyStore(url_);
    }

    const std::string url_{"tcp://127.0.0.1:29819"};
    StorePtr store_;
};

TEST_F(TopologyPreflightTest, PublishSenderWithoutRegisteringMemory)
{
    ASSERT_EQ(PublishTopologyRecord(store_, 0, 0, "TRANS"), SM_OK);
    std::string record;
    ASSERT_EQ(store_->Get("TOPO_0", record, 1000), SM_OK);
    EXPECT_EQ(record, "0:0");
}

TEST_F(TopologyPreflightTest, ReceiverChecksPreviouslyPublishedSender)
{
    ASSERT_EQ(PublishTopologyRecord(store_, 0, 0, "TRANS"), SM_OK);
    EXPECT_EQ(CheckTopologyP2PSlots(store_, 1, 1, 2, 1000, "TRANS"), SM_OK);
}
