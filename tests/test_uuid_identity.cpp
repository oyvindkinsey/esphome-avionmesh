// Tests: unassociated-scan UUID identity decode (vendor/product codes).

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

TEST(UuidIdentityTest, KnownMicroEdge_SampleFromLiveCapture) {
    // UUID_ANNOUNCE bytes captured from a real unassociated device.
    const uint8_t uuid[] = {0xb0, 0xc7, 0x9f, 0xbd, 0xd6, 0x1c, 0x14, 0x00,
                            0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t vendor = 0, product = 0;
    decode_uuid_identity(uuid, sizeof(uuid), vendor, product);
    EXPECT_EQ(vendor, 20);   // Leedarson vendor code
    EXPECT_EQ(product, 18);  // MicroEdge product code (catalog id 162)
}

TEST(UuidIdentityTest, ShortBuffer_YieldsZeros) {
    const uint8_t uuid[] = {0xb0, 0xc7, 0x9f};
    uint8_t vendor = 0xAA, product = 0xAA;
    decode_uuid_identity(uuid, sizeof(uuid), vendor, product);
    EXPECT_EQ(vendor, 0u);
    EXPECT_EQ(product, 0u);
}
