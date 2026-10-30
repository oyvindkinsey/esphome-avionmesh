// Tests: DeviceDB batch mode brackets bulk import without losing data.

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

TEST(DbBatchTest, BatchedAdds_AllPresent) {
    TestHub hub;
    hub.db().begin_batch();
    EXPECT_TRUE(hub.db().add_device(33001, 162, "A"));
    EXPECT_TRUE(hub.db().add_device(33002, 97, "B"));
    EXPECT_TRUE(hub.db().add_group(257, "G"));
    EXPECT_TRUE(hub.db().add_device_to_group(33001, 257));
    hub.db().end_batch();

    EXPECT_NE(hub.db().find_device(33001), nullptr);
    EXPECT_NE(hub.db().find_device(33002), nullptr);
    auto *g = hub.db().find_group(257);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->member_ids.size(), 1u);
}

TEST(DbBatchTest, UnbatchedAdds_StillWork) {
    TestHub hub;
    EXPECT_TRUE(hub.db().add_device(33003, 162, "C"));
    EXPECT_NE(hub.db().find_device(33003), nullptr);
}
