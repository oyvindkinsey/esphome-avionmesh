// RED: tests for exclusive-witness + propagation latching algorithm.
// All tests here fail against the current (all-members-agree) implementation.
//
// Algorithm:
//   When device D reports brightness B:
//   1. candidate_groups = all groups D belongs to
//   2. For each candidate group G:
//      - Find an exclusive witness: member M of G that is NOT in any other
//        candidate group, AND has device_states_[M].brightness == B
//      - If found → G was triggered; latch G to B
//   3. Propagation: for every latched group G, also latch any group H where
//      H.members ⊆ G.members (all of H's devices were also updated by G's command)

#include "mock_hub.h"
#include <gtest/gtest.h>

using namespace avionmesh;

// Device IDs (≥ GROUP_THRESHOLD = 32896)
static constexpr uint16_t A = 32900;
static constexpr uint16_t B = 32901;
static constexpr uint16_t C = 32902;
static constexpr uint16_t D = 32903;

// Group IDs (< GROUP_THRESHOLD)
static constexpr uint16_t G1 = 1000;
static constexpr uint16_t G2 = 1001;
static constexpr uint16_t G3 = 1002;

// ---- Exclusive witness: single-group device ----

// Device in only one group → it is its own exclusive witness → latch immediately,
// no need to wait for other members.
TEST(ExclusiveWitness, SingleGroupDevice_LatchesWithoutWaitingForPeers) {
    TestHub hub;
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_group(G1, "G1");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.test_setup();

    // A is in G1 only → exclusive witness for G1
    hub.inject_brightness(A, 128);

    ASSERT_EQ(hub.states().count(G1), 1u) << "single-group device should latch group immediately";
    EXPECT_EQ(hub.states().at(G1).brightness, 128);
}

// Multi-group device with no exclusive witness yet → no latch.
TEST(ExclusiveWitness, MultiGroupDevice_NoExclusiveWitnessYet_NoLatch) {
    TestHub hub;
    hub.db().add_device(A, 90, "A");  // in G1 and G2
    hub.db().add_device(B, 90, "B");  // in G1 only
    hub.db().add_device(C, 90, "C");  // in G2 only
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    // A is in both G1 and G2; B and C have not reported yet
    hub.inject_brightness(A, 100);

    EXPECT_EQ(hub.states().count(G1), 0u) << "no exclusive witness for G1 yet";
    EXPECT_EQ(hub.states().count(G2), 0u) << "no exclusive witness for G2 yet";
}

// B (exclusive to G1) reports → G1 triggered, G2 untouched.
TEST(ExclusiveWitness, ExclusiveG1MemberReports_LatchesG1Only) {
    TestHub hub;
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_device(C, 90, "C");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    hub.inject_brightness(A, 100);  // ambiguous
    hub.inject_brightness(B, 100);  // B is G1-exclusive → proves G1

    ASSERT_EQ(hub.states().count(G1), 1u);
    EXPECT_EQ(hub.states().at(G1).brightness, 100);
    EXPECT_EQ(hub.states().count(G2), 0u) << "G2 must not latch; no G2-exclusive witness";
}

// C (exclusive to G2) reports → G2 triggered, G1 untouched.
TEST(ExclusiveWitness, ExclusiveG2MemberReports_LatchesG2Only) {
    TestHub hub;
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_device(C, 90, "C");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    hub.inject_brightness(A, 200);
    hub.inject_brightness(C, 200);  // C is G2-exclusive → proves G2

    ASSERT_EQ(hub.states().count(G2), 1u);
    EXPECT_EQ(hub.states().at(G2).brightness, 200);
    EXPECT_EQ(hub.states().count(G1), 0u);
}

// ---- Propagation ----

// G1 ⊆ G2 (G1.members ⊆ G2.members).
// When G2 is triggered (C exclusive to G2 reports), G1 must also be latched.
TEST(Propagation, OuterGroupTriggered_AlsoLatchesSubgroup) {
    TestHub hub;
    // G1 = {A, B}, G2 = {A, B, C}  →  G1 ⊆ G2
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_device(C, 90, "C");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(B, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    hub.inject_brightness(C, 150);  // C exclusive to G2 → G2 triggered

    ASSERT_EQ(hub.states().count(G2), 1u) << "G2 must latch";
    EXPECT_EQ(hub.states().at(G2).brightness, 150);

    ASSERT_EQ(hub.states().count(G1), 1u) << "G1 ⊆ G2: must be latched via propagation";
    EXPECT_EQ(hub.states().at(G1).brightness, 150);
}

// When only an inner-group command fires, the outer group must NOT latch.
TEST(Propagation, InnerGroupTriggered_DoesNotLatchOuter) {
    TestHub hub;
    // G1 = {A},  G2 = {A, C}  (C exclusive to G2)
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(C, 90, "C");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    // A is in G1 and G2; as a G1 command with A being the only G1 member,
    // A IS exclusive to G1 (relative to candidate_groups = {G1, G2}, only G1
    // contains A without C; wait — A IS in G2 too). Let me reconsider…
    // Actually A is in {G1, G2}, so candidate_groups = {G1, G2}.
    // For G1: exclusive witness = member of G1 NOT in G2 = none (A is in both).
    // For G2: exclusive witness = member of G2 NOT in G1 = C (not reported yet).
    // → no latch. Then if G1 = {A} only and A is the ONLY member, A cannot be
    // exclusive because it's also in G2.
    //
    // This represents a topology where G1 has no exclusive members: G1 can
    // only latch via propagation from G2.  Verify G1 does NOT latch on A alone.
    hub.inject_brightness(A, 77);

    EXPECT_EQ(hub.states().count(G1), 0u) << "G1 has no exclusive member; cannot self-latch";
    EXPECT_EQ(hub.states().count(G2), 0u) << "G2 needs C as witness";
}

// Deep nesting: G3 ⊇ G2 ⊇ G1.  D (exclusive to G3) reports → all three latch.
TEST(Propagation, DeepNesting_OutermostTriggered_PropagatesAll) {
    TestHub hub;
    // G1={A,B}, G2={A,B,C}, G3={A,B,C,D}
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_device(C, 90, "C");
    hub.db().add_device(D, 90, "D");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_group(G3, "G3");
    for (auto id : {A, B})          hub.db().add_device_to_group(id, G1);
    for (auto id : {A, B, C})       hub.db().add_device_to_group(id, G2);
    for (auto id : {A, B, C, D})    hub.db().add_device_to_group(id, G3);
    hub.test_setup();

    hub.inject_brightness(D, 42);  // D exclusive to G3 → G3 triggered

    ASSERT_EQ(hub.states().count(G3), 1u); EXPECT_EQ(hub.states().at(G3).brightness, 42);
    ASSERT_EQ(hub.states().count(G2), 1u); EXPECT_EQ(hub.states().at(G2).brightness, 42);
    ASSERT_EQ(hub.states().count(G1), 1u); EXPECT_EQ(hub.states().at(G1).brightness, 42);
}

// Sibling groups sharing a member: G1={A,B}, G2={A,C}.
// B (exclusive to G1) reports → only G1 latches; G2 untouched.
TEST(Propagation, SiblingGroups_OnlyCorrectOneLatches) {
    TestHub hub;
    hub.db().add_device(A, 90, "A");
    hub.db().add_device(B, 90, "B");
    hub.db().add_device(C, 90, "C");
    hub.db().add_group(G1, "G1");
    hub.db().add_group(G2, "G2");
    hub.db().add_device_to_group(A, G1);
    hub.db().add_device_to_group(B, G1);
    hub.db().add_device_to_group(A, G2);
    hub.db().add_device_to_group(C, G2);
    hub.test_setup();

    hub.inject_brightness(B, 99);  // B exclusive to G1

    ASSERT_EQ(hub.states().count(G1), 1u);
    EXPECT_EQ(hub.states().at(G1).brightness, 99);
    EXPECT_EQ(hub.states().count(G2), 0u) << "G2 is a sibling, not a subset of G1";
}
