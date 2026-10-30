// Tests: mesh discovery ping parsing — full vs short replies.

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

class DiscoverPingTest : public ::testing::Test {
 protected:
    TestHub hub;

    void SetUp() override { hub.set_discovering(true); }

    // Deliver a raw payload with an explicit length shorter than the backing
    // buffer; bytes past len are sentinel 0xAA to expose over-reads.
    void inject_raw(uint16_t device_id, const uint8_t *data, size_t len) {
        hub.inject_ping_raw(device_id, data, len);
    }
};

TEST_F(DiscoverPingTest, FullReply_ParsesFwVendorCsr) {
    // payload: Ping(6) + 2 header bytes + fw 1.13.0 + flags + vendor 0x000A + csr 18
    hub.inject_ping(33078, {0x00, 0x00, 0x01, 0x0D, 0x00, 0x02, 0x00, 0x0A, 0x12});

    ASSERT_EQ(hub.discovered().size(), 1u);
    const auto &d = hub.discovered()[0];
    EXPECT_EQ(d.device_id, 33078);
    EXPECT_EQ(d.fw_major, 0x01);
    EXPECT_EQ(d.fw_minor, 0x0D);
    EXPECT_EQ(d.fw_patch, 0x00);
    EXPECT_EQ(d.vendor_id, 0x000A);
    EXPECT_EQ(d.csr_product_id, 0x12);
    EXPECT_FALSE(d.short_reply);
}

TEST_F(DiscoverPingTest, ShortReply_FieldsStayZero) {
    // Smart-dimmer style short reply (7 bytes); sentinel past len must not leak in.
    const uint8_t buf[] = {0x06, 0xFF, 0x00, 0x01, 0x0A, 0x09, 0x00, 0xAA, 0xAA, 0xAA};
    inject_raw(33014, buf, 7);

    ASSERT_EQ(hub.discovered().size(), 1u);
    const auto &d = hub.discovered()[0];
    EXPECT_EQ(d.device_id, 33014);
    EXPECT_EQ(d.fw_major, 0u);
    EXPECT_EQ(d.fw_minor, 0u);
    EXPECT_EQ(d.fw_patch, 0u);
    EXPECT_EQ(d.flags, 0u);
    EXPECT_EQ(d.vendor_id, 0u);
    EXPECT_EQ(d.csr_product_id, 0u);
    EXPECT_TRUE(d.short_reply);
}
