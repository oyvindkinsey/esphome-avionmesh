// Tests: JSON escaping of user-supplied names in SSE payloads.
//
// Device/group names are free text (claim, add, rename). The SSE payloads are
// built by string concatenation, so a name containing a quote or backslash
// used to corrupt every downstream JSON parse in the UI (web.js) and any
// consumer of the stream. append_json_string lives in json_escape.h so it is
// exercisable here without the ESP web server; the E2E tests below drive the
// hub event builders through the deferred-action queue.

#include "json_escape.h"
#include "mock_hub.h"

#include <ArduinoJson.h>
#include <gtest/gtest.h>

using namespace avionmesh;

static std::string esc(const std::string &s) {
    std::string out;
    append_json_string(out, s);
    return out;
}

TEST(JsonEscape, Plain) {
    EXPECT_EQ(esc("kitchen"), "\"kitchen\"");
    EXPECT_EQ(esc(""), "\"\"");
}

TEST(JsonEscape, QuotesAndBackslash) {
    EXPECT_EQ(esc("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(esc("a\\b"), "\"a\\\\b\"");
    EXPECT_EQ(esc("\""), "\"\\\"\"");
}

TEST(JsonEscape, ControlChars) {
    EXPECT_EQ(esc("a\nb"), "\"a\\nb\"");
    EXPECT_EQ(esc("a\rb"), "\"a\\rb\"");
    EXPECT_EQ(esc("a\tb"), "\"a\\tb\"");
    EXPECT_EQ(esc(std::string("a\x01"
                              "b")),
              "\"a\\u0001b\"");
}

TEST(JsonEscape, Utf8Passthrough) {
    EXPECT_EQ(esc("café"), "\"café\"");
}

static const char *kNastyName = "Kitchen \"main\" lamp\\2";

class SseJsonEscapeTest : public ::testing::Test {
 protected:
    TestHub hub;

    void SetUp() override { hub.test_setup(); }

    static void expect_valid_with_name(const std::string &json, const char *key, uint32_t id,
                                       const char *name) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, json);
        ASSERT_FALSE(err) << "payload must parse as JSON: " << json;
        EXPECT_EQ(doc["avion_id"].as<uint32_t>() + doc["group_id"].as<uint32_t>(), id)
            << "payload: " << json;
        EXPECT_STREQ(doc["name"].as<const char *>(), name) << "payload: " << json;
    }
};

TEST_F(SseJsonEscapeTest, AddDiscovered_DeviceAddedEscaped) {
    DeferredAction act;
    act.type = DeferredAction::AddDiscovered;
    act.id1 = 100;
    act.product_type = 90;
    act.name = kNastyName;
    hub.push_action(act);

    auto *ev = hub.last_sse_event("device_added");
    ASSERT_NE(ev, nullptr);
    expect_valid_with_name(ev->second, "avion_id", 100, kNastyName);
}

TEST_F(SseJsonEscapeTest, CreateGroup_GroupAddedEscaped) {
    DeferredAction act;
    act.type = DeferredAction::CreateGroup;
    act.name = kNastyName;
    hub.push_action(act);

    auto *ev = hub.last_sse_event("group_added");
    ASSERT_NE(ev, nullptr);
    expect_valid_with_name(ev->second, "group_id", 256, kNastyName);
}

TEST_F(SseJsonEscapeTest, RenameDevice_DeviceUpdatedEscaped) {
    hub.db().add_device(101, 90, "orig");
    hub.test_setup();

    DeferredAction act;
    act.type = DeferredAction::RenameDevice;
    act.id1 = 101;
    act.name = kNastyName;
    hub.push_action(act);

    auto *ev = hub.last_sse_event("device_updated");
    ASSERT_NE(ev, nullptr);
    expect_valid_with_name(ev->second, "avion_id", 101, kNastyName);
}

TEST_F(SseJsonEscapeTest, RenameGroup_GroupUpdatedEscaped) {
    hub.db().add_group(7, "orig");
    hub.test_setup();

    DeferredAction act;
    act.type = DeferredAction::RenameGroup;
    act.id1 = 7;
    act.name = kNastyName;
    hub.push_action(act);

    auto *ev = hub.last_sse_event("group_updated");
    ASSERT_NE(ev, nullptr);
    expect_valid_with_name(ev->second, "group_id", 7, kNastyName);
}

TEST_F(SseJsonEscapeTest, AddToGroup_GroupUpdatedEscaped) {
    hub.db().add_device(102, 90, "member");
    hub.db().add_group(8, kNastyName);
    hub.test_setup();

    DeferredAction act;
    act.type = DeferredAction::AddToGroup;
    act.id1 = 102;
    act.id2 = 8;
    hub.push_action(act);

    auto *ev = hub.last_sse_event("group_updated");
    ASSERT_NE(ev, nullptr);
    expect_valid_with_name(ev->second, "group_id", 8, kNastyName);
}
